/* The fs module of the palanu API (api/palanu/fs.h). Every path an app gives is first mapped into the app's own
 * namespace: /data is its persistent directory (/apps/<id>/data), /tmp its private one (removed when the app ends),
 * /home needs fs.home and /sd needs fs.media. Any other path, and a ".." that would leave the namespace, is refused with
 * PAL_ERR_PERM. The glue checks the "fs" capability; this file does the mapping, the records, and the handles. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"

#define VDEPTH 16 /* components in one app path, and levels removed from its private /tmp */

/* Values of the constants in api/palanu/fs.h (the app SDK sees those; the system does not include them). */
#define FS_KIND_FILE 1
#define FS_KIND_DIR 2
#define FS_STAT_LEN 16    /* FS_STAT_LEN */
#define FS_DIRENT_LEN 64  /* FS_DIRENT_LEN */
#define FS_DIRENT_NAME 56 /* FS_DIRENT_NAME */
#define FS_SEEK_SET 0
#define FS_SEEK_END 2

/* The first component of an app path picks the root; ROOT_LIST is the app's "/" itself. */
typedef enum { ROOT_DATA, ROOT_TMP, ROOT_HOME, ROOT_MEDIA, ROOT_LIST } root_t;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- directory handles: a nema object of their own, held in the app's handle table ---- */
typedef struct {
    nema_obj_t obj;
    char real[NEMA_PATH_MAX]; /* the directory on the filesystem */
    bool roots;               /* the app's "/": its namespace roots are listed, not a filesystem directory */
    uint32_t index;           /* next entry */
} dir_t;

static int d_read(nema_obj_t *o, void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_ISDIR;
}
static int d_write(nema_obj_t *o, const void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_ISDIR;
}
static uint32_t d_poll(nema_obj_t *o, uint32_t events) {
    (void)o, (void)events;
    return 0;
}
static nema_err_t d_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    (void)o, (void)req, (void)arg;
    return NEMA_ERR_NOSYS;
}
static void d_destroy(nema_obj_t *o) { nema_free(o); } /* the object is the first member: same address */
static const nema_obj_ops_t DIR_OPS = {"dir", d_read, d_write, d_poll, d_ioctl, d_destroy};

/* The directory behind handle h, if the app owns it and it is a directory handle. */
static nema_err_t dir_of(const api_ctx_t *c, int32_t h, dir_t **out) {
    if (!api_fd_owned(c, h)) return NEMA_ERR_BADF;
    nema_obj_t *o = nema_handle_obj(h);
    if (!o || o->ops != &DIR_OPS) return NEMA_ERR_NOTDIR;
    *out = (dir_t *)o;
    return NEMA_OK;
}

static nema_owner_t caller_owner(void) {
    nema_proc_t *p = nema_self();
    return p ? p->owner : NEMA_OWNER_KERNEL;
}

/* ---- the namespace ---- */

/* mkdir -p for a directory the namespace owns (/apps/<id>/data, the private /tmp). */
static nema_err_t ensure_dir(const char *dir) {
    char part[NEMA_PATH_MAX];
    size_t n = strlen(dir);
    if (n >= sizeof(part)) return NEMA_ERR_RANGE;
    for (size_t i = 1; i <= n; i++) {
        if (i < n && dir[i] != '/') continue;
        memcpy(part, dir, i);
        part[i] = 0;
        nema_stat_t st;
        if (nema_fs_stat(part, &st) == NEMA_OK) continue;
        nema_err_t r = nema_fs_mkdir(part);
        if (r != NEMA_OK && r != NEMA_ERR_EXIST) return r;
    }
    return NEMA_OK;
}

/* Normalises buf in place: collapses "//", drops ".", and pops for "..". comp receives the components. A ".." with
 * nothing left to pop would leave the app's "/": refused. */
static nema_err_t split(char *buf, const char **comp, int *nc) {
    *nc = 0;
    for (char *s = buf; *s;) {
        while (*s == '/') s++;
        if (!*s) break;
        char *start = s;
        while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (*nc == 0) return NEMA_ERR_PERM;
            (*nc)--;
            continue;
        }
        if (*nc == VDEPTH) return NEMA_ERR_RANGE;
        comp[(*nc)++] = start;
    }
    return NEMA_OK;
}

/* The real path of an app path, with the namespace applied. A relative path starts in /data. Creates the directory the
 * root needs (/apps/<id>/data, the private /tmp) when it is missing. */
static nema_err_t resolve(const api_ctx_t *c, const char *in, root_t *root, char *real, size_t cap) {
    char buf[NEMA_PATH_MAX + 8];
    size_t n = strlen(in);
    if (n == 0) return NEMA_ERR_INVAL;
    if (in[0] == '/') {
        if (n >= sizeof(buf)) return NEMA_ERR_RANGE;
        memcpy(buf, in, n + 1);
    } else {
        if (n + 6 >= sizeof(buf)) return NEMA_ERR_RANGE;
        memcpy(buf, "/data/", 6);
        memcpy(buf + 6, in, n + 1);
    }
    const char *comp[VDEPTH];
    int nc = 0;
    nema_err_t r = split(buf, comp, &nc);
    if (r != NEMA_OK) return r;
    if (nc == 0) {
        *root = ROOT_LIST;
        real[0] = 0;
        return NEMA_OK;
    }
    char base[NEMA_PATH_MAX];
    if (strcmp(comp[0], "data") == 0) {
        *root = ROOT_DATA;
        if (snprintf(base, sizeof(base), "/apps/%s/data", api_app_id(c)) >= (int)sizeof(base)) return NEMA_ERR_RANGE;
        r = ensure_dir(base);
    } else if (strcmp(comp[0], "tmp") == 0) {
        *root = ROOT_TMP;
        if (strlen(api_tmp_dir(c)) >= sizeof(base)) return NEMA_ERR_RANGE;
        strcpy(base, api_tmp_dir(c));
        r = ensure_dir(base);
    } else if (strcmp(comp[0], "home") == 0) {
        if (!api_perm(c, "fs.home")) return NEMA_ERR_PERM;
        *root = ROOT_HOME;
        strcpy(base, "/home");
    } else if (strcmp(comp[0], "sd") == 0) {
        if (!api_perm(c, "fs.media")) return NEMA_ERR_PERM;
        *root = ROOT_MEDIA;
        strcpy(base, "/sd");
    } else {
        return NEMA_ERR_PERM; /* nothing else is visible to an app */
    }
    if (r != NEMA_OK) return r;
    size_t len = strlen(base);
    if (len >= cap) return NEMA_ERR_RANGE;
    memcpy(real, base, len + 1);
    for (int i = 1; i < nc; i++) {
        size_t l = strlen(comp[i]);
        if (len + 1 + l >= cap) return NEMA_ERR_RANGE;
        real[len++] = '/';
        memcpy(real + len, comp[i], l + 1);
        len += l;
    }
    return NEMA_OK;
}

/* Removes the contents of dir, depth first. Each pass takes the first entry, so removing never skips one. */
static void remove_tree(const char *dir, int depth) {
    nema_dirent_t e;
    while (depth < VDEPTH && nema_fs_readdir(dir, 0, &e) == NEMA_OK) {
        char child[NEMA_PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", dir, e.name) >= (int)sizeof(child)) return;
        if (e.type == NEMA_T_DIR) remove_tree(child, depth + 1);
        if (nema_fs_unlink(child) != NEMA_OK) return;
    }
}

void api_fs_release(api_ctx_t *c) {
    const char *dir = api_tmp_dir(c);
    remove_tree(dir, 0);
    nema_fs_unlink(dir); /* NOENT when the app never used /tmp */
}

/* ---- the calls ---- */

int32_t psys_api_open(api_ctx_t *c, const char *path, uint32_t len, int32_t flags) {
    if (len == 0 || len >= NEMA_PATH_MAX || memchr(path, 0, len) != NULL) return NEMA_ERR_INVAL;
    char p[NEMA_PATH_MAX];
    memcpy(p, path, len);
    p[len] = 0;
    root_t root;
    char real[NEMA_PATH_MAX];
    nema_err_t r = resolve(c, p, &root, real, sizeof(real));
    if (r != NEMA_OK) return r;
    if (root == ROOT_LIST) return NEMA_ERR_ISDIR;
    nema_stat_t st;
    if (nema_fs_stat(real, &st) == NEMA_OK && st.type == NEMA_T_DIR) return NEMA_ERR_ISDIR;
    if (!api_fd_has_slot(c)) return NEMA_ERR_FULL;
    uint32_t want = (uint32_t)flags & (NEMA_O_READ | NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC | NEMA_O_APPEND);
    int fd = nema_open(real, want);
    if (fd >= 0) api_fd_add(c, fd);
    return fd;
}

int32_t psys_api_close(api_ctx_t *c, int32_t fd) {
    if (!api_fd_remove(c, fd)) return NEMA_ERR_BADF;
    return nema_close(fd);
}

int32_t psys_api_stat(api_ctx_t *c, const char *path, void *st_out, uint32_t len) {
    if (len < FS_STAT_LEN) return NEMA_ERR_INVAL;
    root_t root;
    char real[NEMA_PATH_MAX];
    nema_err_t r = resolve(c, path, &root, real, sizeof(real));
    if (r != NEMA_OK) return r;
    uint32_t size = 0, mtime = 0, kind = FS_KIND_DIR;
    if (root != ROOT_LIST) {
        nema_stat_t st;
        r = nema_fs_stat(real, &st);
        if (r != NEMA_OK) return r;
        size = st.size;
        mtime = st.mtime;
        kind = st.type == NEMA_T_DIR ? FS_KIND_DIR : FS_KIND_FILE;
    }
    uint8_t *o = (uint8_t *)st_out;
    put32(o, size);
    put32(o + 4, mtime);
    put32(o + 8, kind);
    put32(o + 12, 0);
    return NEMA_OK;
}

int32_t psys_api_dir_open(api_ctx_t *c, const char *path) {
    if (!api_fd_has_slot(c)) return NEMA_ERR_FULL;
    root_t root;
    char real[NEMA_PATH_MAX];
    nema_err_t r = resolve(c, path, &root, real, sizeof(real));
    if (r != NEMA_OK) return r;
    if (root != ROOT_LIST) {
        nema_stat_t st;
        r = nema_fs_stat(real, &st);
        if (r != NEMA_OK) return r;
        if (st.type != NEMA_T_DIR) return NEMA_ERR_NOTDIR;
    }
    dir_t *d = (dir_t *)nema_calloc(NEMA_MEM_ANY, caller_owner(), sizeof(*d));
    if (!d) return NEMA_ERR_NOMEM;
    nema_obj_init(&d->obj, &DIR_OPS);
    d->roots = root == ROOT_LIST;
    nema_strlcpy(d->real, real, sizeof(d->real));
    int fd = nema_handle_install(&d->obj);
    nema_obj_unref(&d->obj); /* the handle table holds the reference now */
    if (fd >= 0) api_fd_add(c, fd);
    return fd;
}

/* The app's "/" lists data and tmp, and home and sd when they are granted. */
static bool root_name(const api_ctx_t *c, uint32_t i, const char **name) {
    static const char *const ALWAYS[] = {"data", "tmp"};
    if (i < 2) {
        *name = ALWAYS[i];
        return true;
    }
    i -= 2;
    if (api_perm(c, "fs.home")) {
        if (i == 0) {
            *name = "home";
            return true;
        }
        i--;
    }
    if (api_perm(c, "fs.media") && i == 0) {
        *name = "sd";
        return true;
    }
    return false;
}

int32_t psys_api_dir_next(api_ctx_t *c, int32_t dh, void *ent_out, uint32_t len) {
    if (len < FS_DIRENT_LEN) return NEMA_ERR_INVAL;
    dir_t *d = NULL;
    nema_err_t r = dir_of(c, dh, &d);
    if (r != NEMA_OK) return r;
    char name[NEMA_NAME_MAX];
    uint32_t size = 0, kind = FS_KIND_FILE;
    if (d->roots) {
        const char *n = NULL;
        if (!root_name(c, d->index, &n)) return 0;
        nema_strlcpy(name, n, sizeof(name));
        kind = FS_KIND_DIR;
    } else {
        nema_dirent_t e;
        r = nema_fs_readdir(d->real, d->index, &e);
        if (r == NEMA_ERR_NOENT) return 0;
        if (r != NEMA_OK) return r;
        nema_strlcpy(name, e.name, sizeof(name));
        kind = e.type == NEMA_T_DIR ? FS_KIND_DIR : FS_KIND_FILE;
        size = e.size;
    }
    d->index++;
    uint8_t *o = (uint8_t *)ent_out;
    memset(o, 0, FS_DIRENT_LEN);
    put32(o, size);
    put32(o + 4, kind);
    nema_strlcpy((char *)o + 8, name, FS_DIRENT_NAME);
    return 1;
}

int64_t psys_api_seek(api_ctx_t *c, int32_t h, int64_t off, int32_t whence) {
    if (!api_fd_owned(c, h)) return NEMA_ERR_BADF;
    nema_obj_t *o = nema_handle_obj(h);
    if (!o) return NEMA_ERR_BADF;
    if (o->ops == &DIR_OPS) return NEMA_ERR_ISDIR;
    if (whence < FS_SEEK_SET || whence > FS_SEEK_END) return NEMA_ERR_INVAL;
    if (off < INT32_MIN || off > INT32_MAX) return NEMA_ERR_RANGE;
    nema_seek_t sk = {(int32_t)off, whence};
    nema_err_t r = nema_ioctl(h, NEMA_FIOC_SEEK, &sk);
    if (r != NEMA_OK) return r;
    return sk.off;
}

int32_t psys_api_mkdir(api_ctx_t *c, const char *path) {
    root_t root;
    char real[NEMA_PATH_MAX];
    nema_err_t r = resolve(c, path, &root, real, sizeof(real));
    if (r != NEMA_OK) return r;
    if (root == ROOT_LIST) return NEMA_ERR_EXIST;
    return nema_fs_mkdir(real);
}

int32_t psys_api_remove(api_ctx_t *c, const char *path) {
    root_t root;
    char real[NEMA_PATH_MAX];
    nema_err_t r = resolve(c, path, &root, real, sizeof(real));
    if (r != NEMA_OK) return r;
    if (root == ROOT_LIST) return NEMA_ERR_PERM;
    return nema_fs_unlink(real);
}

int32_t psys_api_rename(api_ctx_t *c, const char *from, const char *to) {
    root_t ra, rb;
    char a[NEMA_PATH_MAX], b[NEMA_PATH_MAX];
    nema_err_t r = resolve(c, from, &ra, a, sizeof(a));
    if (r != NEMA_OK) return r;
    r = resolve(c, to, &rb, b, sizeof(b));
    if (r != NEMA_OK) return r;
    if (ra == ROOT_LIST || rb == ROOT_LIST) return NEMA_ERR_PERM;
    return nema_fs_rename(a, b);
}

int32_t psys_api_fsync(api_ctx_t *c, int32_t h) {
    if (!api_fd_owned(c, h)) return NEMA_ERR_BADF;
    nema_obj_t *o = nema_handle_obj(h);
    if (!o) return NEMA_ERR_BADF;
    if (o->ops == &DIR_OPS) return NEMA_ERR_ISDIR;
    return nema_ioctl(h, NEMA_FIOC_SYNC, NULL);
}
