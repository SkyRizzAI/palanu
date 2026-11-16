#include <string.h>

#include "internal.h"
#include "nema/vfs.h"

/* An in-memory filesystem. Data lives in PSRAM; a byte budget keeps a runaway writer from eating it all. */
typedef struct node {
    char name[NEMA_NAME_MAX];
    uint8_t type;
    struct node *parent, *child, *next;
    uint8_t *data;
    uint32_t size, cap, mtime;
    int refs;      /* open file objects */
    bool detached; /* unlinked while open: freed on last close */
} node_t;

typedef struct {
    nema_port_mutex_t *lock;
    node_t *root;
    size_t used, max;
    nema_owner_t owner;
} ramfs_t;

typedef struct {
    nema_obj_t obj;
    ramfs_t *fs;
    node_t *n;
    uint32_t pos, flags;
} file_t;

static uint32_t now_s(void) { return (uint32_t)(nema_port_time_us() / 1000000u); }

static node_t *node_new(ramfs_t *fs, const char *name, uint8_t type) {
    node_t *n = (node_t *)nema_calloc(NEMA_MEM_ANY, fs->owner, sizeof(*n));
    if (!n) return NULL;
    nema_strlcpy(n->name, name, sizeof(n->name));
    n->type = type;
    n->mtime = now_s();
    return n;
}

static void node_free(ramfs_t *fs, node_t *n) {
    fs->used -= n->cap;
    nema_free(n->data);
    nema_free(n);
}

/* Look up an absolute path inside the filesystem. parent_out receives the parent when the final component
 * is missing (so create can attach to it); leaf receives the final component name. */
static node_t *lookup(ramfs_t *fs, const char *path, node_t **parent_out, char *leaf) {
    node_t *cur = fs->root;
    if (parent_out) *parent_out = NULL;
    if (leaf) leaf[0] = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[NEMA_NAME_MAX];
        size_t l = 0;
        while (p[l] && p[l] != '/') l++;
        if (l >= sizeof(comp)) return NULL;
        memcpy(comp, p, l);
        comp[l] = 0;
        p += l;
        while (*p == '/') p++;
        if (cur->type != NEMA_T_DIR) return NULL;
        node_t *c = cur->child;
        while (c && strcmp(c->name, comp) != 0) c = c->next;
        if (!c) {
            if (!*p && parent_out) {
                *parent_out = cur;
                if (leaf) nema_strlcpy(leaf, comp, NEMA_NAME_MAX);
            }
            return NULL;
        }
        cur = c;
    }
    return cur;
}

static void attach(node_t *parent, node_t *n) {
    n->parent = parent;
    n->next = NULL;
    if (!parent->child) {
        parent->child = n;
    } else {
        node_t *t = parent->child;
        while (t->next) t = t->next;
        t->next = n;
    }
    parent->mtime = now_s();
}

static void detach(node_t *n) {
    node_t **pp = &n->parent->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    n->parent = NULL;
}

/* ---- file objects ---- */
static int f_read(nema_obj_t *o, void *buf, size_t n) {
    file_t *f = (file_t *)o;
    if (!(f->flags & NEMA_O_READ)) return NEMA_ERR_PERM;
    nema_port_mutex_lock(f->fs->lock);
    uint32_t avail = f->pos < f->n->size ? f->n->size - f->pos : 0;
    size_t take = n < avail ? n : avail;
    if (take) memcpy(buf, f->n->data + f->pos, take);
    f->pos += (uint32_t)take;
    nema_port_mutex_unlock(f->fs->lock);
    return (int)take;
}

static int f_write(nema_obj_t *o, const void *buf, size_t n) {
    file_t *f = (file_t *)o;
    if (!(f->flags & NEMA_O_WRITE)) return NEMA_ERR_PERM;
    ramfs_t *fs = f->fs;
    nema_port_mutex_lock(fs->lock);
    if (f->flags & NEMA_O_APPEND) f->pos = f->n->size;
    uint64_t end = (uint64_t)f->pos + n;
    int r = (int)n;
    if (end > f->n->cap) {
        uint32_t ncap = f->n->cap ? f->n->cap : 64;
        while (ncap < end) ncap *= 2;
        if (fs->used + (ncap - f->n->cap) > fs->max) {
            r = NEMA_ERR_NOSPC;
        } else {
            uint8_t *nd = (uint8_t *)nema_alloc(NEMA_MEM_ANY, fs->owner, ncap);
            if (!nd) {
                r = NEMA_ERR_NOMEM;
            } else {
                if (f->n->size) memcpy(nd, f->n->data, f->n->size);
                nema_free(f->n->data);
                fs->used += ncap - f->n->cap;
                f->n->data = nd;
                f->n->cap = ncap;
            }
        }
    }
    if (r > 0) {
        if (f->pos > f->n->size) memset(f->n->data + f->n->size, 0, f->pos - f->n->size);
        memcpy(f->n->data + f->pos, buf, n);
        f->pos += (uint32_t)n;
        if (f->pos > f->n->size) f->n->size = f->pos;
        f->n->mtime = now_s();
    }
    nema_port_mutex_unlock(fs->lock);
    return r;
}

static uint32_t f_poll(nema_obj_t *o, uint32_t ev) {
    (void)o, (void)ev;
    return NEMA_POLL_IN | NEMA_POLL_OUT;
}

static nema_err_t f_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    file_t *f = (file_t *)o;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(f->fs->lock);
    if (req == NEMA_FIOC_SIZE) {
        *(uint32_t *)arg = f->n->size;
    } else if (req == NEMA_FIOC_SYNC) {
        r = NEMA_OK; /* RAM: a write is already in its final place */
    } else if (req == NEMA_FIOC_SEEK) {
        nema_seek_t *s = (nema_seek_t *)arg;
        int64_t base = s->whence == 1 ? (int64_t)f->pos : s->whence == 2 ? (int64_t)f->n->size : 0;
        int64_t np = base + s->off;
        if (np < 0) r = NEMA_ERR_RANGE;
        else {
            f->pos = (uint32_t)np;
            s->off = (int32_t)np;
        }
    } else {
        r = NEMA_ERR_NOSYS;
    }
    nema_port_mutex_unlock(f->fs->lock);
    return r;
}

static void f_destroy(nema_obj_t *o) {
    file_t *f = (file_t *)o;
    ramfs_t *fs = f->fs;
    nema_port_mutex_lock(fs->lock);
    if (--f->n->refs == 0 && f->n->detached) node_free(fs, f->n);
    nema_port_mutex_unlock(fs->lock);
    nema_free(f);
}

static const nema_obj_ops_t FILE_OPS = {"file", f_read, f_write, f_poll, f_ioctl, f_destroy};

/* ---- filesystem operations ---- */
static nema_err_t r_open(void *vfs, const char *path, uint32_t flags, nema_obj_t **out) {
    ramfs_t *fs = (ramfs_t *)vfs;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(fs->lock);
    node_t *parent, *n;
    char leaf[NEMA_NAME_MAX];
    n = lookup(fs, path, &parent, leaf);
    if (!n) {
        if (!(flags & NEMA_O_CREATE) || !parent || !leaf[0]) {
            r = NEMA_ERR_NOENT;
        } else {
            n = node_new(fs, leaf, NEMA_T_FILE);
            if (!n) r = NEMA_ERR_NOMEM;
            else attach(parent, n);
        }
    } else if (n->type == NEMA_T_DIR) {
        r = NEMA_ERR_ISDIR;
    } else if (flags & NEMA_O_TRUNC) {
        n->size = 0;
    }
    if (r == NEMA_OK) {
        file_t *f = (file_t *)nema_calloc(NEMA_MEM_ANY, fs->owner, sizeof(*f));
        if (!f) {
            r = NEMA_ERR_NOMEM;
        } else {
            nema_obj_init(&f->obj, &FILE_OPS);
            f->obj.flags = NEMA_OBJ_ATOMIC_WRITE; /* a kill never cuts a file write short */
            f->fs = fs;
            f->n = n;
            f->flags = flags & (NEMA_O_READ | NEMA_O_WRITE | NEMA_O_APPEND);
            n->refs++;
            *out = &f->obj;
        }
    }
    nema_port_mutex_unlock(fs->lock);
    return r;
}

static nema_err_t r_stat(void *vfs, const char *path, nema_stat_t *st) {
    ramfs_t *fs = (ramfs_t *)vfs;
    nema_port_mutex_lock(fs->lock);
    node_t *n = lookup(fs, path, NULL, NULL);
    if (n) {
        st->type = n->type;
        st->size = n->size;
        st->mtime = n->mtime;
    }
    nema_port_mutex_unlock(fs->lock);
    return n ? NEMA_OK : NEMA_ERR_NOENT;
}

static nema_err_t r_readdir(void *vfs, const char *path, size_t index, nema_dirent_t *ent) {
    ramfs_t *fs = (ramfs_t *)vfs;
    nema_err_t r = NEMA_ERR_NOENT;
    nema_port_mutex_lock(fs->lock);
    node_t *d = lookup(fs, path, NULL, NULL);
    if (!d) {
        r = NEMA_ERR_NOENT;
    } else if (d->type != NEMA_T_DIR) {
        r = NEMA_ERR_NOTDIR;
    } else {
        node_t *c = d->child;
        for (size_t i = 0; c && i < index; i++) c = c->next;
        if (c) {
            nema_strlcpy(ent->name, c->name, sizeof(ent->name));
            ent->type = c->type;
            ent->size = c->size;
            r = NEMA_OK;
        }
    }
    nema_port_mutex_unlock(fs->lock);
    return r;
}

static nema_err_t r_mkdir(void *vfs, const char *path) {
    ramfs_t *fs = (ramfs_t *)vfs;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(fs->lock);
    node_t *parent, *n;
    char leaf[NEMA_NAME_MAX];
    n = lookup(fs, path, &parent, leaf);
    if (n) r = NEMA_ERR_EXIST;
    else if (!parent || !leaf[0]) r = NEMA_ERR_NOENT;
    else {
        n = node_new(fs, leaf, NEMA_T_DIR);
        if (!n) r = NEMA_ERR_NOMEM;
        else attach(parent, n);
    }
    nema_port_mutex_unlock(fs->lock);
    return r;
}

static nema_err_t r_unlink(void *vfs, const char *path) {
    ramfs_t *fs = (ramfs_t *)vfs;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(fs->lock);
    node_t *n = lookup(fs, path, NULL, NULL);
    if (!n) r = NEMA_ERR_NOENT;
    else if (n == fs->root) r = NEMA_ERR_BUSY;
    else if (n->type == NEMA_T_DIR && n->child) r = NEMA_ERR_STATE;
    else {
        detach(n);
        if (n->refs > 0) n->detached = true;
        else node_free(fs, n);
    }
    nema_port_mutex_unlock(fs->lock);
    return r;
}

static nema_err_t r_rename(void *vfs, const char *from, const char *to) {
    ramfs_t *fs = (ramfs_t *)vfs;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(fs->lock);
    node_t *n = lookup(fs, from, NULL, NULL);
    node_t *np, *dst;
    char leaf[NEMA_NAME_MAX];
    dst = lookup(fs, to, &np, leaf);
    if (!n || n == fs->root) r = NEMA_ERR_NOENT;
    else if (dst == n) r = NEMA_OK;
    else if (dst) {
        if (dst->type == NEMA_T_DIR || n->type == NEMA_T_DIR) r = NEMA_ERR_EXIST;
        else { /* replace the target file */
            node_t *dp = dst->parent;
            detach(dst);
            if (dst->refs > 0) dst->detached = true;
            else node_free(fs, dst);
            detach(n);
            nema_strlcpy(n->name, dst->name, sizeof(n->name));
            attach(dp, n);
        }
    } else if (!np || !leaf[0]) {
        r = NEMA_ERR_NOENT;
    } else {
        for (node_t *a = np; a; a = a->parent)
            if (a == n) r = NEMA_ERR_INVAL; /* moving a directory into itself */
        if (r == NEMA_OK) {
            detach(n);
            nema_strlcpy(n->name, leaf, sizeof(n->name));
            attach(np, n);
        }
    }
    nema_port_mutex_unlock(fs->lock);
    return r;
}

static nema_err_t r_statfs(void *vfs, uint64_t *total, uint64_t *free_bytes) {
    ramfs_t *fs = (ramfs_t *)vfs;
    if (total) *total = fs->max;
    if (free_bytes) *free_bytes = fs->max > fs->used ? fs->max - fs->used : 0;
    return NEMA_OK;
}

static const nema_fs_ops_t RAMFS = {"ramfs", r_open, r_stat, r_readdir, r_mkdir, r_unlink, r_rename, r_statfs};
const nema_fs_ops_t *nema_ramfs_ops(void) { return &RAMFS; }

void *nema_ramfs_create(size_t max_bytes) {
    nema_owner_t owner = nema_owner_register("ramfs");
    ramfs_t *fs = (ramfs_t *)nema_calloc(NEMA_MEM_ANY, owner, sizeof(*fs));
    if (!fs) return NULL;
    fs->lock = nema_port_mutex_create();
    fs->owner = owner;
    fs->max = max_bytes;
    fs->root = node_new(fs, "", NEMA_T_DIR);
    if (!fs->lock || !fs->root) return NULL;
    return fs;
}
