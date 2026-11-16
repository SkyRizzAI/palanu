#include <stdio.h>
#include <string.h>

#include "internal.h"

/* Every block carries an 8-byte header right below the user pointer:
 *   size (24 bits), class (2), owner (6: 0..63, see mem.h), distance back to the real block start (16), magic (16).
 * Blocks of process owners carry one more link right below the header: the intrusive list that the exit sweep walks.
 * Kernel and subsystem blocks have no link, so they pay nothing for it. */
typedef struct {
    uint32_t size : 24;
    uint32_t cls : 2;
    uint32_t owner : 6;
    uint16_t offset;
    uint16_t magic;
} hdr_t;

typedef struct blink {
    struct blink *prev, *next;
} blink_t;

_Static_assert(sizeof(hdr_t) == 8, "header must stay 8 bytes");

#define LINK_BYTES ((sizeof(blink_t) + 7u) & ~7u)
#define PROC_HDR_BYTES (LINK_BYTES + sizeof(hdr_t))

#define HDR_MAGIC 0xA11Cu
#define SIZE_MAX_BLOCK 0xFFFFFFu
#define MAX_RECLAIMERS 4
#define BIG_REQUEST (4u * 1024u)  /* requests this size or more must fit in one free block */
#define BLOCK_MARGIN (2u * 1024u) /* free block beyond the request itself: slack for fragmentation */
/* The 2-bit class field cannot hold ANY; ANY resolves to a real class before the header is written. */

typedef struct {
    bool used;
    char name[16];
    uint32_t cls_used[NEMA_MEM_CLASS_COUNT];
    uint32_t stack, peak, allocs, fails;
    uint32_t quota_ext, quota_int; /* process owners only; 0 = no quota */
    blink_t *blocks;               /* process owners only: every live heap block, newest first */
} owner_t;

static struct {
    nema_port_mutex_t *lock; /* guards everything here and the admission decision (floor, block, quota, alloc) */
    size_t floor;
    size_t reserved; /* internal bytes promised to task stacks that are being created */
    owner_t owners[NEMA_OWNER_MAX];
    struct {
        nema_reclaim_fn fn;
        void *ctx;
        int prio;
    } reclaim[MAX_RECLAIMERS];
    bool ready;
} M;

static inline bool is_internal(nema_mem_class_t c) { return c == NEMA_MEM_INTERNAL || c == NEMA_MEM_DMA; }
static inline bool is_proc(nema_owner_t o) { return o >= NEMA_OWNER_PROC_BASE && o < NEMA_OWNER_MAX; }
static inline size_t hdr_bytes(nema_owner_t o) { return is_proc(o) ? PROC_HDR_BYTES : sizeof(hdr_t); }
static inline blink_t *link_of(void *user) { return (blink_t *)(void *)((uint8_t *)user - sizeof(hdr_t) - LINK_BYTES); }
static inline size_t avail_locked(size_t free_bytes) { return free_bytes > M.reserved ? free_bytes - M.reserved : 0; }

void nema_mem_init(size_t floor);
void nema_mem_init(size_t floor) {
    memset(&M, 0, sizeof(M));
    M.lock = nema_port_mutex_create();
    M.floor = floor ? floor : NEMA_MEM_FLOOR_DEFAULT;
    M.owners[0].used = true;
    strcpy(M.owners[0].name, "kernel");
    for (int i = 0; i < NEMA_OWNER_MAX - NEMA_OWNER_PROC_BASE; i++) {
        M.owners[NEMA_OWNER_PROC_BASE + i].used = true;
        snprintf(M.owners[NEMA_OWNER_PROC_BASE + i].name, sizeof(M.owners[0].name), "proc%d", i);
    }
    M.ready = true;
}

nema_owner_t nema_owner_register(const char *name) {
    nema_owner_t found = 0;
    nema_port_mutex_lock(M.lock);
    for (int i = 1; i < NEMA_OWNER_PROC_BASE; i++) {
        if (M.owners[i].used && strncmp(M.owners[i].name, name, sizeof(M.owners[i].name) - 1) == 0) {
            found = (nema_owner_t)i;
            break;
        }
    }
    for (int i = 1; !found && i < NEMA_OWNER_PROC_BASE; i++) {
        if (!M.owners[i].used) {
            M.owners[i].used = true;
            nema_strlcpy(M.owners[i].name, name, sizeof(M.owners[i].name));
            found = (nema_owner_t)i;
        }
    }
    nema_port_mutex_unlock(M.lock);
    return found;
}

void nema_owner_set_name(nema_owner_t owner, const char *name) {
    if (owner >= NEMA_OWNER_MAX) return;
    nema_port_mutex_lock(M.lock);
    nema_strlcpy(M.owners[owner].name, name, sizeof(M.owners[owner].name));
    M.owners[owner].name[sizeof(M.owners[owner].name) - 1] = 0;
    nema_port_mutex_unlock(M.lock);
}

nema_owner_t nema_mem_shared_owner(nema_owner_t owner) {
    if (!is_proc(owner)) return owner;
    nema_owner_t ipc = nema_owner_register("ipc");
    return ipc ? ipc : NEMA_OWNER_KERNEL;
}

nema_err_t nema_mem_add_reclaimer(nema_reclaim_fn fn, void *ctx, int priority) {
    nema_err_t r = NEMA_ERR_FULL;
    nema_port_mutex_lock(M.lock);
    for (int i = 0; i < MAX_RECLAIMERS; i++) {
        if (!M.reclaim[i].fn) {
            M.reclaim[i].fn = fn;
            M.reclaim[i].ctx = ctx;
            M.reclaim[i].prio = priority;
            r = NEMA_OK;
            break;
        }
    }
    nema_port_mutex_unlock(M.lock);
    return r;
}

void nema_mem_set_floor(size_t bytes) {
    nema_port_mutex_lock(M.lock);
    M.floor = bytes;
    nema_port_mutex_unlock(M.lock);
}

size_t nema_mem_floor(void) {
    nema_port_mutex_lock(M.lock);
    size_t f = M.floor;
    nema_port_mutex_unlock(M.lock);
    return f;
}

bool nema_mem_internal_can_spare(size_t bytes) {
    size_t free_int = 0;
    nema_port_mutex_lock(M.lock);
    nema_port_mem_stats(NEMA_MEM_INTERNAL, &free_int, NULL, NULL);
    bool ok = avail_locked(free_int) >= bytes + M.floor;
    nema_port_mutex_unlock(M.lock);
    return ok;
}

/* Run reclaimers, lowest priority number first, until `wanted` bytes came back.
 * Called without M.lock: a reclaimer frees memory, which takes the lock. */
static size_t run_reclaimers(size_t wanted) {
    struct {
        nema_reclaim_fn fn;
        void *ctx;
        int prio;
    } local[MAX_RECLAIMERS];
    int n = 0;
    nema_port_mutex_lock(M.lock);
    for (int i = 0; i < MAX_RECLAIMERS; i++) {
        if (!M.reclaim[i].fn) continue;
        local[n].fn = M.reclaim[i].fn;
        local[n].ctx = M.reclaim[i].ctx;
        local[n].prio = M.reclaim[i].prio;
        n++;
    }
    nema_port_mutex_unlock(M.lock);
    for (int i = 1; i < n; i++) { /* insertion sort by priority */
        for (int j = i; j > 0 && local[j].prio < local[j - 1].prio; j--) {
            __typeof__(local[0]) t = local[j];
            local[j] = local[j - 1];
            local[j - 1] = t;
        }
    }
    size_t got = 0;
    for (int i = 0; i < n && got < wanted; i++) got += local[i].fn(local[i].ctx, wanted - got);
    return got;
}

static void account_locked(nema_owner_t owner, nema_mem_class_t cls, int64_t delta_bytes, int delta_allocs) {
    owner_t *o = &M.owners[owner];
    o->cls_used[cls] = (uint32_t)((int64_t)o->cls_used[cls] + delta_bytes);
    o->allocs = (uint32_t)((int)o->allocs + delta_allocs);
    uint32_t total = 0;
    for (int c = 0; c < NEMA_MEM_CLASS_COUNT; c++) total += o->cls_used[c];
    if (total > o->peak) o->peak = total;
}

static void count_fail(nema_owner_t owner) {
    nema_port_mutex_lock(M.lock);
    M.owners[owner].fails++;
    nema_port_mutex_unlock(M.lock);
}

/* ---- admission ---- */
enum { WHY_NOMEM, WHY_QUOTA, WHY_FLOOR, WHY_BLOCK };
typedef struct {
    size_t want; /* bytes a reclaimer should try to free */
    int why;
} refusal_t;

static bool quota_refuses(const owner_t *o, nema_mem_class_t c, size_t size) {
    if (c == NEMA_MEM_EXTERNAL) return o->quota_ext && (size_t)o->cls_used[c] + size > o->quota_ext;
    size_t in = (size_t)o->cls_used[NEMA_MEM_INTERNAL] + o->cls_used[NEMA_MEM_DMA];
    return o->quota_int && in + size > o->quota_int;
}

/* Caller holds M.lock. Checks the quota, the floor, and the largest block, then allocates and books the block.
 * On refusal returns NULL and fills `r`. */
static void *admit_locked(nema_mem_class_t c, nema_owner_t owner, size_t size, size_t align, uint32_t flags,
                          refusal_t *r) {
    size_t off = align > hdr_bytes(owner) ? align : hdr_bytes(owner);
    size_t total = size + off;
    owner_t *o = &M.owners[owner];
    if (quota_refuses(o, c, size)) {
        r->why = WHY_QUOTA;
        return NULL;
    }
    if (is_internal(c) && !(flags & NEMA_ALLOC_CRITICAL)) {
        size_t free_int = 0, largest = 0;
        nema_port_mem_stats(NEMA_MEM_INTERNAL, &free_int, NULL, c == NEMA_MEM_INTERNAL ? &largest : NULL);
        size_t avail = avail_locked(free_int);
        if (avail < total + M.floor) {
            r->why = WHY_FLOOR;
            r->want = total + M.floor - avail;
            return NULL;
        }
        if (size >= BIG_REQUEST) {
            if (c != NEMA_MEM_INTERNAL) nema_port_mem_stats(c, NULL, NULL, &largest);
            if (largest < total + BLOCK_MARGIN) {
                r->why = WHY_BLOCK;
                r->want = total + BLOCK_MARGIN - largest;
                return NULL;
            }
        }
    }
    uint8_t *block = nema_port_alloc(c, total, align > sizeof(hdr_t) ? align : 0);
    if (!block) {
        r->why = WHY_NOMEM;
        r->want = total;
        return NULL;
    }
    if (is_internal(c) && !(flags & NEMA_ALLOC_CRITICAL)) {
        /* The port charges the usable size, which malloc may round up, so the check above is only an estimate. The
         * authoritative test is the free memory after this block is really taken; if that breaks the floor, undo. */
        size_t free_int = 0;
        nema_port_mem_stats(NEMA_MEM_INTERNAL, &free_int, NULL, NULL);
        size_t avail = avail_locked(free_int);
        if (avail < M.floor) {
            nema_port_free(c, block);
            r->why = WHY_FLOOR;
            r->want = M.floor - avail;
            return NULL;
        }
    }
    uint8_t *user = block + off;
    hdr_t *h = (hdr_t *)(void *)(user - sizeof(hdr_t));
    h->size = (uint32_t)size;
    h->cls = c;
    h->owner = owner;
    h->offset = (uint16_t)off;
    h->magic = HDR_MAGIC;
    if (is_proc(owner)) {
        blink_t *l = link_of(user);
        l->prev = NULL;
        l->next = o->blocks;
        if (o->blocks) o->blocks->prev = l;
        o->blocks = l;
    }
    account_locked(owner, c, (int64_t)size, 1);
    return user;
}

/* One class, with reclaimers between a refusal and a retry. A quota refusal never reclaims. */
static void *alloc_class(nema_mem_class_t c, nema_owner_t owner, size_t size, size_t align, uint32_t flags) {
    if (nema_port_mem_total(c) == 0) return NULL;
    for (int pass = 0;; pass++) {
        refusal_t r = {0, WHY_NOMEM};
        nema_port_mutex_lock(M.lock);
        void *user = admit_locked(c, owner, size, align, flags, &r);
        nema_port_mutex_unlock(M.lock);
        if (user || r.why == WHY_QUOTA || pass > 0) return user;
        run_reclaimers(r.want);
    }
}

void *nema_alloc_ex(nema_mem_class_t cls, nema_owner_t owner, size_t size, size_t align, uint32_t flags) {
    if (owner >= NEMA_OWNER_MAX) owner = NEMA_OWNER_KERNEL;
    if (size == 0 || size > SIZE_MAX_BLOCK - 64) {
        count_fail(owner);
        return NULL;
    }
    nema_mem_class_t order[2];
    int n = 0;
    if (cls == NEMA_MEM_ANY) {
        order[n++] = NEMA_MEM_EXTERNAL;
        order[n++] = NEMA_MEM_INTERNAL;
    } else {
        order[n++] = cls;
    }
    for (int i = 0; i < n; i++) {
        void *user = alloc_class(order[i], owner, size, align, flags);
        if (!user) continue;
        if (flags & NEMA_ALLOC_ZERO) memset(user, 0, size);
        return user;
    }
    count_fail(owner);
    return NULL;
}

/* Caller holds M.lock. Unlinks and un-books a live block; returns its real start and class for the port. */
static void release_locked(void *user, void **block, nema_mem_class_t *cls) {
    hdr_t *h = (hdr_t *)(void *)((uint8_t *)user - sizeof(hdr_t));
    nema_owner_t owner = (nema_owner_t)h->owner;
    *cls = (nema_mem_class_t)h->cls;
    *block = (uint8_t *)user - h->offset;
    int64_t size = h->size;
    h->magic = 0;
    if (is_proc(owner)) {
        blink_t *l = link_of(user);
        if (l->prev) l->prev->next = l->next;
        else M.owners[owner].blocks = l->next;
        if (l->next) l->next->prev = l->prev;
    }
    account_locked(owner, *cls, -size, -1);
}

void nema_free(void *p) {
    if (!p) return;
    hdr_t *h = (hdr_t *)(void *)((uint8_t *)p - sizeof(hdr_t));
    void *block = NULL;
    nema_mem_class_t cls = NEMA_MEM_INTERNAL;
    nema_port_mutex_lock(M.lock);
    bool live = h->magic == HDR_MAGIC;
    if (live) release_locked(p, &block, &cls);
    nema_port_mutex_unlock(M.lock);
    if (!live) nema_panic("nema_free: bad pointer %p", p);
    nema_port_free(cls, block);
}

size_t nema_alloc_size(const void *p) {
    if (!p) return 0;
    const hdr_t *h = (const hdr_t *)(const void *)((const uint8_t *)p - sizeof(hdr_t));
    return h->magic == HDR_MAGIC ? h->size : 0;
}

/* ---- process owners ---- */
void nema_mem_owner_begin(nema_owner_t owner, size_t quota_ext, size_t quota_int) {
    if (owner >= NEMA_OWNER_MAX) return;
    nema_port_mutex_lock(M.lock);
    owner_t *o = &M.owners[owner];
    o->quota_ext = quota_ext > UINT32_MAX ? UINT32_MAX : (uint32_t)quota_ext;
    o->quota_int = quota_int > UINT32_MAX ? UINT32_MAX : (uint32_t)quota_int;
    o->peak = 0;
    o->fails = 0;
    nema_port_mutex_unlock(M.lock);
}

size_t nema_mem_owner_sweep(nema_owner_t owner, size_t *blocks) {
    size_t bytes = 0, count = 0;
    if (owner < NEMA_OWNER_MAX) {
        for (;;) {
            nema_port_mutex_lock(M.lock);
            blink_t *l = M.owners[owner].blocks;
            if (!l) {
                nema_port_mutex_unlock(M.lock);
                break;
            }
            void *user = (uint8_t *)l + LINK_BYTES + sizeof(hdr_t);
            void *block = NULL;
            nema_mem_class_t cls = NEMA_MEM_INTERNAL;
            bytes += ((hdr_t *)(void *)((uint8_t *)user - sizeof(hdr_t)))->size;
            count++;
            release_locked(user, &block, &cls);
            nema_port_mutex_unlock(M.lock);
            nema_port_free(cls, block);
        }
    }
    if (blocks) *blocks = count;
    return bytes;
}

/* ---- task stacks ---- */
bool nema_mem_task_reserve(size_t bytes, bool gated) {
    for (int pass = 0;; pass++) {
        size_t want = 0;
        nema_port_mutex_lock(M.lock);
        bool ok = true;
        if (gated) {
            size_t free_int = 0;
            nema_port_mem_stats(NEMA_MEM_INTERNAL, &free_int, NULL, NULL);
            size_t avail = avail_locked(free_int);
            if (avail < bytes + M.floor) {
                ok = false;
                want = bytes + M.floor - avail;
            }
        }
        if (ok) M.reserved += bytes;
        nema_port_mutex_unlock(M.lock);
        if (ok) return true;
        if (pass > 0) return false;
        run_reclaimers(want);
    }
}

void nema_mem_task_release(size_t bytes) {
    nema_port_mutex_lock(M.lock);
    M.reserved = M.reserved >= bytes ? M.reserved - bytes : 0;
    nema_port_mutex_unlock(M.lock);
}

void nema_mem_stack_account(nema_owner_t owner, size_t bytes, bool add) {
    if (owner >= NEMA_OWNER_MAX) owner = NEMA_OWNER_KERNEL;
    nema_port_mutex_lock(M.lock);
    if (add) M.owners[owner].stack += (uint32_t)bytes;
    else M.owners[owner].stack -= (M.owners[owner].stack >= bytes) ? (uint32_t)bytes : M.owners[owner].stack;
    nema_port_mutex_unlock(M.lock);
}

/* ---- reports ---- */
bool nema_mem_owner_stat(nema_owner_t owner, nema_owner_stat_t *out) {
    if (owner >= NEMA_OWNER_MAX) return false;
    bool ok;
    nema_port_mutex_lock(M.lock);
    owner_t *o = &M.owners[owner];
    ok = o->used;
    if (ok) {
        memcpy(out->name, o->name, sizeof(out->name));
        memcpy(out->used, o->cls_used, sizeof(out->used));
        out->stack = o->stack;
        out->peak = o->peak;
        out->allocs = o->allocs;
        out->fails = o->fails;
        out->quota_ext = o->quota_ext;
        out->quota_int = o->quota_int;
    }
    nema_port_mutex_unlock(M.lock);
    return ok;
}

/* "2048K/64K" for a process with quotas, "-" for everything else. */
static void quota_text(const nema_owner_stat_t *s, char *b, size_t n) {
    if (!s->quota_ext) snprintf(b, n, "-");
    else snprintf(b, n, "%luK/%luK", (unsigned long)(s->quota_ext / 1024), (unsigned long)(s->quota_int / 1024));
}

size_t nema_mem_report(char *buf, size_t cap) {
    size_t n = 0;
    static const struct {
        nema_mem_class_t c;
        const char *name;
    } classes[] = {{NEMA_MEM_INTERNAL, "internal"}, {NEMA_MEM_DMA, "dma"}, {NEMA_MEM_EXTERNAL, "external"}};
#define APPEND(...)                                                                                          \
    do {                                                                                                     \
        if (n < cap) n += (size_t)snprintf(buf + n, cap - n, __VA_ARGS__);                                   \
    } while (0)
    APPEND("%-9s %7s %7s %8s %8s\n", "memory", "total", "free", "largest", "min free");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(classes); i++) {
        nema_mem_class_t c = classes[i].c;
        if (c == NEMA_MEM_DMA) continue; /* DMA memory is a subset of internal; not listed separately */
        size_t total = nema_port_mem_total(c);
        if (!total) continue;
        APPEND("%-9s %6zuK %6zuK %7zuK %7zuK\n", classes[i].name, total / 1024, nema_port_mem_free(c) / 1024,
               nema_port_mem_largest(c) / 1024, nema_port_mem_min_free(c) / 1024);
    }
    APPEND("floor     %6zuK kept free for radio and DMA\n", nema_mem_floor() / 1024);
    APPEND("\n%-10s %6s %7s %6s %6s %5s %4s %10s\n", "owner", "int", "ext", "stack", "peak", "allocs", "fail", "quota");
    for (int i = 0; i < NEMA_OWNER_MAX; i++) {
        nema_owner_stat_t s;
        if (!nema_mem_owner_stat((nema_owner_t)i, &s)) continue;
        uint32_t in = s.used[NEMA_MEM_INTERNAL] + s.used[NEMA_MEM_DMA];
        if (i >= NEMA_OWNER_PROC_BASE && !in && !s.used[NEMA_MEM_EXTERNAL] && !s.stack && !s.peak) continue;
        char q[24];
        quota_text(&s, q, sizeof(q));
        APPEND("%-10s %5.1fK %6.1fK %5.1fK %5.1fK %5lu %4lu %10s\n", s.name, (double)in / 1024.0,
               (double)s.used[NEMA_MEM_EXTERNAL] / 1024.0, (double)s.stack / 1024.0, (double)s.peak / 1024.0,
               (unsigned long)s.allocs, (unsigned long)s.fails, q);
    }
#undef APPEND
    return n;
}

/* Detailed report for the `mem` command: each class with its free bytes, low-water mark and largest free block, then
 * the owners holding memory in exact bytes. DMA is part of internal on the board, so its row is folded into the owner
 * columns as int+dma. */
size_t nema_mem_report_ex(char *buf, size_t cap) {
    size_t n = 0;
    static const struct {
        nema_mem_class_t c;
        const char *name;
    } classes[] = {{NEMA_MEM_INTERNAL, "internal"}, {NEMA_MEM_DMA, "dma"}, {NEMA_MEM_EXTERNAL, "external"}};
#define APPEND(...)                                                                                          \
    do {                                                                                                     \
        if (n < cap) n += (size_t)snprintf(buf + n, cap - n, __VA_ARGS__);                                   \
    } while (0)
    APPEND("%-9s %6s %6s %6s %6s\n", "memory", "total", "free", "min", "block");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(classes); i++) {
        nema_mem_class_t c = classes[i].c;
        size_t total = nema_port_mem_total(c);
        if (!total) {
            APPEND("%-9s absent\n", classes[i].name);
            continue;
        }
        size_t free_now = 0, min_free = 0, largest = 0;
        nema_port_mem_stats((int)c, &free_now, &min_free, &largest);
        APPEND("%-9s %5zuK %5zuK %5zuK %5zuK\n", classes[i].name, total / 1024, free_now / 1024, min_free / 1024,
               largest / 1024);
    }
    APPEND("floor %zuK kept free; dma is part of internal\n", nema_mem_floor() / 1024);
    APPEND("\n%-10s %9s %9s %8s %7s %10s\n", "owner", "int+dma", "external", "stack", "allocs", "quota");
    for (int i = 0; i < NEMA_OWNER_MAX; i++) {
        nema_owner_stat_t s;
        if (!nema_mem_owner_stat((nema_owner_t)i, &s)) continue;
        uint32_t inner = s.used[NEMA_MEM_INTERNAL] + s.used[NEMA_MEM_DMA];
        if (!inner && !s.used[NEMA_MEM_EXTERNAL] && !s.stack && !s.allocs) continue;
        char q[24];
        quota_text(&s, q, sizeof(q));
        APPEND("%-10s %9lu %9lu %8lu %7lu %10s\n", s.name, (unsigned long)inner, (unsigned long)s.used[NEMA_MEM_EXTERNAL],
               (unsigned long)s.stack, (unsigned long)s.allocs, q);
    }
#undef APPEND
    return n;
}
