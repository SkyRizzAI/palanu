/* Nema port for the host (macOS/Linux): pthreads and malloc.
 *
 * The memory pools are simulated so admission and floor logic behave like the board:
 * INTERNAL is 459 KB (what the hhv2 board really has free), EXTERNAL is 16 MB.
 * Task stacks are charged to the internal pool, because on the board they come from there.
 */
#include <errno.h>
#if !defined(__APPLE__)
#include <sys/random.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define USABLE(p) malloc_size(p)
#else
#include <malloc.h>
#define USABLE(p) malloc_usable_size(p)
#endif

#include "nema/port.h"

static size_t pool_total[2] = {459u * 1024u, 16u * 1024u * 1024u}; /* [0] internal, [1] external */
static size_t pool_used[2];
static size_t pool_min_free[2] = {459u * 1024u, 16u * 1024u * 1024u};
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t pool_once = PTHREAD_ONCE_INIT;

static void pool_init(void) {
    const char *kb = getenv("NEMA_HOST_INTERNAL_KB");
    if (kb && atoi(kb) > 0) {
        pool_total[0] = (size_t)atoi(kb) * 1024u;
        pool_min_free[0] = pool_total[0];
    }
}

static int pool_of(nema_mem_class_t c) { return c == NEMA_MEM_EXTERNAL ? 1 : 0; }

static void pool_note(int pi) {
    size_t free_now = pool_total[pi] - pool_used[pi];
    if (free_now < pool_min_free[pi]) pool_min_free[pi] = free_now;
}

void *nema_port_alloc(nema_mem_class_t cls, size_t size, size_t align) {
    pthread_once(&pool_once, pool_init);
    int pi = pool_of(cls);
    pthread_mutex_lock(&pool_lock);
    if (pool_used[pi] + size > pool_total[pi]) {
        pthread_mutex_unlock(&pool_lock);
        return NULL;
    }
    pool_used[pi] += size; /* reserve, then true up to the real usable size */
    pthread_mutex_unlock(&pool_lock);

    void *p = NULL;
    if (align > sizeof(void *)) {
        if (posix_memalign(&p, align, size) != 0) p = NULL;
    } else {
        p = malloc(size);
    }
    pthread_mutex_lock(&pool_lock);
    if (!p) {
        pool_used[pi] -= size;
    } else {
        size_t real = USABLE(p);
        pool_used[pi] = pool_used[pi] - size + real;
        pool_note(pi);
    }
    pthread_mutex_unlock(&pool_lock);
    return p;
}

void nema_port_free(nema_mem_class_t cls, void *p) {
    if (!p) return;
    int pi = pool_of(cls);
    size_t real = USABLE(p);
    pthread_mutex_lock(&pool_lock);
    pool_used[pi] -= real;
    pthread_mutex_unlock(&pool_lock);
    free(p);
}

size_t nema_port_mem_total(nema_mem_class_t cls) {
    pthread_once(&pool_once, pool_init);
    return pool_total[pool_of(cls)];
}
size_t nema_port_mem_free(nema_mem_class_t cls) {
    pthread_once(&pool_once, pool_init);
    int pi = pool_of(cls);
    pthread_mutex_lock(&pool_lock);
    size_t f = pool_total[pi] - pool_used[pi];
    pthread_mutex_unlock(&pool_lock);
    return f;
}
size_t nema_port_mem_largest(nema_mem_class_t cls) { return nema_port_mem_free(cls); }
size_t nema_port_mem_min_free(nema_mem_class_t cls) {
    int pi = pool_of(cls);
    pthread_mutex_lock(&pool_lock);
    size_t f = pool_min_free[pi];
    pthread_mutex_unlock(&pool_lock);
    return f;
}
bool nema_port_heap_check(void) { return true; }

void nema_port_mem_stats(int cls, size_t *free, size_t *min_free, size_t *largest) {
    /* No heap introspection on the host: the largest block is simulated as three quarters of the free bytes. */
    size_t f = nema_port_mem_free((nema_mem_class_t)cls);
    if (free) *free = f;
    if (min_free) *min_free = nema_port_mem_min_free((nema_mem_class_t)cls);
    if (largest) *largest = f - f / 4;
}

/* ---- time ---- */
uint64_t nema_port_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void nema_port_random(void *buf, size_t len) {
#if defined(__APPLE__)
    arc4random_buf(buf, len); /* the kernel's entropy pool; never fails */
#else
    uint8_t *p = buf;
    while (len > 0) {
        size_t n = len < 256 ? len : 256; /* getentropy: at most 256 bytes per call */
        if (getentropy(p, n) != 0) abort();
        p += n, len -= n;
    }
#endif
}

void nema_port_sleep_ms(uint32_t ms) {
    struct timespec ts = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- scheduling jitter (debug aid) ----
 * NEMA_HOST_JITTER=<microseconds> sleeps a random time before lock and semaphore operations, which widens
 * race windows enough for the unit tests to expose ordering bugs without a debugger. */
static int jitter_max = -1;
static __thread unsigned jitter_state = 2463534242u;
static void jitter(void) {
    if (jitter_max < 0) {
        const char *e = getenv("NEMA_HOST_JITTER");
        jitter_max = e ? atoi(e) : 0;
    }
    if (jitter_max <= 0) return;
    jitter_state ^= jitter_state << 13;
    jitter_state ^= jitter_state >> 17;
    jitter_state ^= jitter_state << 5;
    if ((jitter_state & 3u) == 0) usleep(jitter_state % (unsigned)jitter_max);
}

/* ---- mutex ---- */
struct nema_port_mutex {
    pthread_mutex_t m;
};
nema_port_mutex_t *nema_port_mutex_create(void) {
    nema_port_mutex_t *m = malloc(sizeof(*m));
    if (m) pthread_mutex_init(&m->m, NULL);
    return m;
}
void nema_port_mutex_lock(nema_port_mutex_t *m) {
    jitter();
    pthread_mutex_lock(&m->m);
}
void nema_port_mutex_unlock(nema_port_mutex_t *m) { pthread_mutex_unlock(&m->m); }
void nema_port_mutex_destroy(nema_port_mutex_t *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

/* ---- counting semaphore ---- */
struct nema_port_sem {
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t count, max;
};
nema_port_sem_t *nema_port_sem_create(uint32_t initial, uint32_t max) {
    nema_port_sem_t *s = malloc(sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->count = initial;
    s->max = max;
    return s;
}
nema_err_t nema_port_sem_take(nema_port_sem_t *s, int32_t timeout_ms) {
    nema_err_t r = NEMA_OK;
    struct timespec abs;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &abs);
        abs.tv_sec += timeout_ms / 1000;
        abs.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (abs.tv_nsec >= 1000000000L) {
            abs.tv_sec++;
            abs.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&s->m);
    while (s->count == 0) {
        if (timeout_ms == 0) {
            r = NEMA_ERR_TIMEOUT;
            break;
        }
        if (timeout_ms < 0) {
            pthread_cond_wait(&s->c, &s->m);
        } else if (pthread_cond_timedwait(&s->c, &s->m, &abs) == ETIMEDOUT && s->count == 0) {
            r = NEMA_ERR_TIMEOUT;
            break;
        }
    }
    if (r == NEMA_OK) s->count--;
    pthread_mutex_unlock(&s->m);
    return r;
}
void nema_port_sem_give(nema_port_sem_t *s) {
    jitter();
    pthread_mutex_lock(&s->m);
    if (s->count < s->max) s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}
void nema_port_sem_destroy(nema_port_sem_t *s) {
    if (!s) return;
    pthread_mutex_destroy(&s->m);
    pthread_cond_destroy(&s->c);
    free(s);
}

/* ---- tasks ---- */
struct nema_port_task {
    pthread_t th;
};
typedef struct {
    nema_port_task_fn fn;
    void *arg;
    size_t stack;
} start_t;

static __thread void *tls_ptr;
static __thread size_t my_stack;

static void *thread_main(void *p) {
    start_t *st = (start_t *)p;
    nema_port_task_fn fn = st->fn;
    void *arg = st->arg;
    my_stack = st->stack;
    free(st);
    fn(arg);
    nema_port_task_exit();
}

nema_err_t nema_port_task_create(nema_port_task_t **out, const char *name, nema_port_task_fn fn, void *arg,
                                 size_t stack_bytes, int prio, int core) {
    (void)name, (void)prio, (void)core;
    pthread_once(&pool_once, pool_init);
    start_t *st = malloc(sizeof(*st));
    if (!st) return NEMA_ERR_NOMEM;
    st->fn = fn;
    st->arg = arg;
    st->stack = stack_bytes;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    size_t real = stack_bytes * 4 < 256u * 1024u ? 256u * 1024u : stack_bytes * 4; /* host frames are bigger */
    pthread_attr_setstacksize(&attr, real);
    pthread_mutex_lock(&pool_lock);
    pool_used[0] += stack_bytes;
    pool_note(0);
    pthread_mutex_unlock(&pool_lock);
    pthread_t th;
    int rc = pthread_create(&th, &attr, thread_main, st);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&pool_lock);
        pool_used[0] -= stack_bytes;
        pthread_mutex_unlock(&pool_lock);
        free(st);
        return NEMA_ERR_NOMEM;
    }
    if (out) *out = NULL; /* detached: the handle is not needed on the host */
    return NEMA_OK;
}

void nema_port_task_exit(void) {
    pthread_mutex_lock(&pool_lock);
    pool_used[0] -= my_stack;
    pthread_mutex_unlock(&pool_lock);
    pthread_exit(NULL);
}

size_t nema_port_task_stack_free(void) { return 4096; }
void *nema_port_tls_get(void) { return tls_ptr; }
void nema_port_tls_set(void *p) { tls_ptr = p; }

int nema_port_cpu_count(void) { return 1; }
int nema_port_cpu_percent(nema_cpu_sample_t *s, int core) {
    (void)s, (void)core;
    return 0;
}

void nema_port_console_write(const char *buf, size_t n) {
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
}

void nema_port_abort(void) { abort(); }
