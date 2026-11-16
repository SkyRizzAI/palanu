#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "nema/vfs.h"

#define MOUNT_MAX 8

typedef struct {
    char path[NEMA_PATH_MAX]; /* "/" or "/sd" (no trailing slash) */
    const nema_fs_ops_t *ops;
    void *fs;
} mount_t;

static struct {
    nema_port_mutex_t *lock;
    mount_t m[MOUNT_MAX];
    int n;
} V;

void nema_vfs_init(void);
void nema_vfs_init(void) {
    memset(&V, 0, sizeof(V));
    V.lock = nema_port_mutex_create();
}

nema_err_t nema_vfs_mount(const char *mountpoint, const nema_fs_ops_t *ops, void *fs) {
    if (!mountpoint || mountpoint[0] != '/' || !ops) return NEMA_ERR_INVAL;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(V.lock);
    for (int i = 0; i < V.n; i++)
        if (strcmp(V.m[i].path, mountpoint) == 0) r = NEMA_ERR_EXIST;
    if (r == NEMA_OK && V.n == MOUNT_MAX) r = NEMA_ERR_FULL;
    if (r == NEMA_OK) {
        mount_t *m = &V.m[V.n++];
        nema_strlcpy(m->path, mountpoint, sizeof(m->path));
        size_t l = strlen(m->path);
        if (l > 1 && m->path[l - 1] == '/') m->path[l - 1] = 0;
        m->ops = ops;
        m->fs = fs;
    }
    nema_port_mutex_unlock(V.lock);
    return r;
}

nema_err_t nema_vfs_unmount(const char *mountpoint) {
    nema_err_t r = NEMA_ERR_NOENT;
    nema_port_mutex_lock(V.lock);
    for (int i = 0; i < V.n; i++) {
        if (strcmp(V.m[i].path, mountpoint) == 0) {
            V.m[i] = V.m[--V.n];
            r = NEMA_OK;
            break;
        }
    }
    nema_port_mutex_unlock(V.lock);
    return r;
}

/* ---- path handling ---- */
nema_err_t nema_path_resolve(const char *path, char *out, size_t cap) {
    char tmp[NEMA_PATH_MAX * 2];
    if (!path || !*path) return NEMA_ERR_INVAL;
    nema_proc_t *p = nema_self();
    if (path[0] == '/') {
        nema_strlcpy(tmp, path, sizeof(tmp));
    } else {
        const char *cwd = p ? p->cwd : "/";
        if (strlen(cwd) + strlen(path) + 2 > sizeof(tmp)) return NEMA_ERR_RANGE;
        strcpy(tmp, cwd);
        strcat(tmp, "/");
        strcat(tmp, path);
    }
    /* collapse "//", ".", and ".." in place */
    char *comp[32];
    int nc = 0;
    for (char *s = tmp; *s;) {
        while (*s == '/') *s++ = 0;
        if (!*s) break;
        char *start = s;
        while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (nc > 0) nc--;
            continue;
        }
        if (nc == 32) return NEMA_ERR_RANGE;
        comp[nc++] = start;
    }
    size_t n = 0;
    if (cap < 2) return NEMA_ERR_RANGE;
    out[n++] = '/';
    for (int i = 0; i < nc; i++) {
        size_t l = strlen(comp[i]);
        if (n + l + 2 > cap) return NEMA_ERR_RANGE;
        memcpy(out + n, comp[i], l);
        n += l;
        if (i + 1 < nc) out[n++] = '/';
    }
    out[n] = 0;
    return NEMA_OK;
}

/* Find the longest mount that is a prefix of the absolute path; sub receives the path inside the mount. */
static const mount_t *route(const char *abs, const char **sub, mount_t *copy) {
    nema_port_mutex_lock(V.lock);
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < V.n; i++) {
        size_t l = strlen(V.m[i].path);
        bool root = l == 1;
        if (strncmp(abs, V.m[i].path, l) == 0 && (root || abs[l] == 0 || abs[l] == '/') && l >= best_len) {
            best = i;
            best_len = l;
        }
    }
    if (best >= 0) *copy = V.m[best];
    nema_port_mutex_unlock(V.lock);
    if (best < 0) return NULL;
    *sub = copy->path[1] == 0 ? abs : (abs[best_len] ? abs + best_len : "/");
    return copy;
}

/* Resolve path to an absolute path, find its mount, and copy the mount entry (safe against unmount). */
static nema_err_t locate(const char *path, char *abs, size_t cap, const char **sub, mount_t *mc) {
    nema_err_t r = nema_path_resolve(path, abs, cap);
    if (r != NEMA_OK) return r;
    return route(abs, sub, mc) ? NEMA_OK : NEMA_ERR_NOENT;
}

nema_err_t nema_fs_open(const char *path, uint32_t flags, nema_obj_t **out) {
    char abs[NEMA_PATH_MAX];
    const char *sub;
    mount_t m;
    nema_err_t r = locate(path, abs, sizeof(abs), &sub, &m);
    if (r != NEMA_OK) return r;
    return m.ops->open ? m.ops->open(m.fs, sub, flags, out) : NEMA_ERR_NOSYS;
}

nema_err_t nema_fs_stat(const char *path, nema_stat_t *st) {
    char abs[NEMA_PATH_MAX];
    const char *sub;
    mount_t m;
    nema_err_t r = locate(path, abs, sizeof(abs), &sub, &m);
    if (r != NEMA_OK) return r;
    return m.ops->stat ? m.ops->stat(m.fs, sub, st) : NEMA_ERR_NOSYS;
}

nema_err_t nema_fs_mkdir(const char *path) {
    char abs[NEMA_PATH_MAX];
    const char *sub;
    mount_t m;
    nema_err_t r = locate(path, abs, sizeof(abs), &sub, &m);
    if (r != NEMA_OK) return r;
    return m.ops->mkdir ? m.ops->mkdir(m.fs, sub) : NEMA_ERR_NOSYS;
}

nema_err_t nema_fs_unlink(const char *path) {
    char abs[NEMA_PATH_MAX];
    const char *sub;
    mount_t m;
    nema_err_t r = locate(path, abs, sizeof(abs), &sub, &m);
    if (r != NEMA_OK) return r;
    return m.ops->unlink ? m.ops->unlink(m.fs, sub) : NEMA_ERR_NOSYS;
}

/* A directory listing also shows the mount points that live directly inside it. */
nema_err_t nema_fs_readdir(const char *path, size_t index, nema_dirent_t *ent) {
    char abs[NEMA_PATH_MAX];
    nema_err_t pr = nema_path_resolve(path, abs, sizeof(abs));
    if (pr != NEMA_OK) return pr;
    const char *sub;
    mount_t mc;
    const mount_t *m = route(abs, &sub, &mc);
    if (!m) return NEMA_ERR_NOENT;
    nema_err_t r = m->ops->readdir ? m->ops->readdir(m->fs, sub, index, ent) : NEMA_ERR_NOSYS;
    if (r != NEMA_ERR_NOENT) return r;
    /* past the end of the filesystem's own entries: list mounts whose parent is this directory */
    size_t base = 0;
    for (size_t i = 0;; i++) {
        nema_dirent_t tmp;
        if (!m->ops->readdir || m->ops->readdir(m->fs, sub, i, &tmp) != NEMA_OK) {
            base = i;
            break;
        }
    }
    size_t want = index - base, seen = 0;
    size_t alen = strlen(abs);
    nema_port_mutex_lock(V.lock);
    for (int i = 0; i < V.n; i++) {
        const char *mp = V.m[i].path;
        if (strlen(mp) <= 1) continue;
        bool inside = alen == 1 ? true : (strncmp(mp, abs, alen) == 0 && mp[alen] == '/');
        if (!inside) continue;
        const char *rest = mp + (alen == 1 ? 1 : alen + 1);
        if (strchr(rest, '/')) continue;
        /* the parent filesystem may hold a stub directory for the mount point: list it once */
        char child[NEMA_PATH_MAX];
        nema_stat_t st;
        snprintf(child, sizeof(child), "%s%s%s", sub, strcmp(sub, "/") == 0 ? "" : "/", rest);
        if (m->ops->stat && m->ops->stat(m->fs, child, &st) == NEMA_OK) continue;
        if (seen++ == want) {
            nema_strlcpy(ent->name, rest, sizeof(ent->name));
            ent->type = NEMA_T_DIR;
            ent->size = 0;
            nema_port_mutex_unlock(V.lock);
            return NEMA_OK;
        }
    }
    nema_port_mutex_unlock(V.lock);
    return NEMA_ERR_NOENT;
}

nema_err_t nema_fs_rename(const char *from, const char *to) {
    char a[NEMA_PATH_MAX], b[NEMA_PATH_MAX];
    nema_err_t r = nema_path_resolve(from, a, sizeof(a));
    if (r != NEMA_OK) return r;
    r = nema_path_resolve(to, b, sizeof(b));
    if (r != NEMA_OK) return r;
    const char *sa, *sb;
    mount_t ma, mb;
    if (!route(a, &sa, &ma) || !route(b, &sb, &mb)) return NEMA_ERR_NOENT;
    if (ma.ops != mb.ops || ma.fs != mb.fs) return NEMA_ERR_UNSUPPORTED; /* across filesystems: copy and delete */
    return ma.ops->rename ? ma.ops->rename(ma.fs, sa, sb) : NEMA_ERR_NOSYS;
}

nema_err_t nema_fs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes) {
    char a[NEMA_PATH_MAX];
    nema_err_t r = nema_path_resolve(path, a, sizeof(a));
    if (r != NEMA_OK) return r;
    const char *sub;
    mount_t copy;
    const mount_t *m = route(a, &sub, &copy);
    if (!m) return NEMA_ERR_NOENT;
    if (!m->ops->statfs) return NEMA_ERR_NOSYS;
    return m->ops->statfs(m->fs, total, free_bytes);
}

int nema_open(const char *path, uint32_t flags) {
    nema_obj_t *o;
    nema_err_t r = nema_fs_open(path, flags, &o);
    if (r != NEMA_OK) return r;
    int fd = nema_handle_install(o);
    nema_obj_unref(o);
    return fd;
}

nema_err_t nema_set_cwd(const char *path) {
    nema_proc_t *p = nema_self();
    if (!p) return NEMA_ERR_STATE;
    char abs[NEMA_PATH_MAX];
    nema_err_t r = nema_path_resolve(path, abs, sizeof(abs));
    if (r != NEMA_OK) return r;
    nema_stat_t st;
    r = nema_fs_stat(abs, &st);
    if (r != NEMA_OK) return r;
    if (st.type != NEMA_T_DIR) return NEMA_ERR_NOTDIR;
    nema_strlcpy(p->cwd, abs, sizeof(p->cwd));
    return NEMA_OK;
}
