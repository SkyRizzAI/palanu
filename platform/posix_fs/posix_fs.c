/* A Nema filesystem over the POSIX file API, rooted at a directory.
 * Used for LittleFS on the board (via ESP-IDF's VFS), a folder on the host, and later the SD card. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "nema/nema.h"

typedef struct {
    char root[192];
    nema_owner_t owner;
} pfs_t;

typedef struct {
    nema_obj_t obj;
    int fd;
} pfile_t;

static int map_errno(int e) {
    switch (e) {
    case ENOENT: return NEMA_ERR_NOENT;
    case EEXIST: return NEMA_ERR_EXIST;
    case EISDIR: return NEMA_ERR_ISDIR;
    case ENOTDIR: return NEMA_ERR_NOTDIR;
    case ENOSPC: return NEMA_ERR_NOSPC;
    case ENOMEM: return NEMA_ERR_NOMEM;
    case EACCES: case EPERM: return NEMA_ERR_PERM;
    case ENOTEMPTY: return NEMA_ERR_STATE;
    default: return NEMA_ERR_IO;
    }
}

static bool full_path(const pfs_t *fs, const char *path, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", fs->root, path[0] == '/' && path[1] == 0 ? "" : path);
    return n > 0 && (size_t)n < cap;
}

static int f_read(nema_obj_t *o, void *buf, size_t n) {
    ssize_t r = read(((pfile_t *)o)->fd, buf, n);
    return r >= 0 ? (int)r : map_errno(errno);
}
static int f_write(nema_obj_t *o, const void *buf, size_t n) {
    ssize_t r = write(((pfile_t *)o)->fd, buf, n);
    return r >= 0 ? (int)r : map_errno(errno);
}
static uint32_t f_poll(nema_obj_t *o, uint32_t ev) { (void)o, (void)ev; return NEMA_POLL_IN | NEMA_POLL_OUT; }
static nema_err_t f_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    int fd = ((pfile_t *)o)->fd;
    if (req == NEMA_FIOC_SIZE) {
        struct stat st;
        if (fstat(fd, &st) != 0) return map_errno(errno);
        *(uint32_t *)arg = (uint32_t)st.st_size;
        return NEMA_OK;
    }
    if (req == NEMA_FIOC_SYNC) {
        return fsync(fd) == 0 ? NEMA_OK : map_errno(errno);
    }
    if (req == NEMA_FIOC_SEEK) {
        nema_seek_t *s = (nema_seek_t *)arg;
        off_t p = lseek(fd, s->off, s->whence == 1 ? SEEK_CUR : s->whence == 2 ? SEEK_END : SEEK_SET);
        if (p < 0) return map_errno(errno);
        s->off = (int32_t)p;
        return NEMA_OK;
    }
    return NEMA_ERR_NOSYS;
}
static void f_destroy(nema_obj_t *o) {
    close(((pfile_t *)o)->fd);
    nema_free(o);
}
static const nema_obj_ops_t PFILE_OPS = {"pfile", f_read, f_write, f_poll, f_ioctl, f_destroy};

static nema_err_t p_open(void *vfs, const char *path, uint32_t flags, nema_obj_t **out) {
    pfs_t *fs = (pfs_t *)vfs;
    char full[320];
    if (!full_path(fs, path, full, sizeof(full))) return NEMA_ERR_RANGE;
    int of = (flags & NEMA_O_READ) && (flags & NEMA_O_WRITE) ? O_RDWR : (flags & NEMA_O_WRITE) ? O_WRONLY : O_RDONLY;
    if (flags & NEMA_O_CREATE) of |= O_CREAT;
    if (flags & NEMA_O_TRUNC) of |= O_TRUNC;
    if (flags & NEMA_O_APPEND) of |= O_APPEND;
    int fd = open(full, of, 0666);
    if (fd < 0) return map_errno(errno);
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        return NEMA_ERR_ISDIR;
    }
    pfile_t *f = (pfile_t *)nema_calloc(NEMA_MEM_ANY, fs->owner, sizeof(*f));
    if (!f) {
        close(fd);
        return NEMA_ERR_NOMEM;
    }
    nema_obj_init(&f->obj, &PFILE_OPS);
    f->obj.flags = NEMA_OBJ_ATOMIC_WRITE; /* a kill never cuts a file write short */
    f->fd = fd;
    *out = &f->obj;
    return NEMA_OK;
}

static nema_err_t p_stat(void *vfs, const char *path, nema_stat_t *st) {
    char full[320];
    struct stat s;
    if (!full_path((pfs_t *)vfs, path, full, sizeof(full))) return NEMA_ERR_RANGE;
    if (stat(full, &s) != 0) return map_errno(errno);
    st->type = S_ISDIR(s.st_mode) ? NEMA_T_DIR : NEMA_T_FILE;
    st->size = (uint32_t)s.st_size;
    st->mtime = (uint32_t)s.st_mtime;
    return NEMA_OK;
}

static nema_err_t p_readdir(void *vfs, const char *path, size_t index, nema_dirent_t *ent) {
    char full[320];
    if (!full_path((pfs_t *)vfs, path, full, sizeof(full))) return NEMA_ERR_RANGE;
    DIR *d = opendir(full);
    if (!d) return map_errno(errno);
    struct dirent *e;
    size_t i = 0;
    nema_err_t r = NEMA_ERR_NOENT;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (i++ != index) continue;
        nema_strlcpy(ent->name, e->d_name, sizeof(ent->name));
        char child[sizeof(full) + sizeof(e->d_name) + 2];
        struct stat s;
        snprintf(child, sizeof(child), "%s/%s", full, e->d_name);
        ent->type = NEMA_T_FILE;
        ent->size = 0;
        if (stat(child, &s) == 0) {
            ent->type = S_ISDIR(s.st_mode) ? NEMA_T_DIR : NEMA_T_FILE;
            ent->size = (uint32_t)s.st_size;
        }
        r = NEMA_OK;
        break;
    }
    closedir(d);
    return r;
}

static nema_err_t p_mkdir(void *vfs, const char *path) {
    char full[320];
    if (!full_path((pfs_t *)vfs, path, full, sizeof(full))) return NEMA_ERR_RANGE;
    return mkdir(full, 0777) == 0 ? NEMA_OK : map_errno(errno);
}

static nema_err_t p_unlink(void *vfs, const char *path) {
    char full[320];
    struct stat s;
    if (!full_path((pfs_t *)vfs, path, full, sizeof(full))) return NEMA_ERR_RANGE;
    if (stat(full, &s) != 0) return map_errno(errno);
    int r = S_ISDIR(s.st_mode) ? rmdir(full) : unlink(full);
    return r == 0 ? NEMA_OK : map_errno(errno);
}

static nema_err_t p_rename(void *vfs, const char *from, const char *to) {
    char a[320], b[320];
    if (!full_path((pfs_t *)vfs, from, a, sizeof(a)) || !full_path((pfs_t *)vfs, to, b, sizeof(b))) return NEMA_ERR_RANGE;
    return rename(a, b) == 0 ? NEMA_OK : map_errno(errno);
}

static nema_err_t p_statfs(void *vfs, uint64_t *total, uint64_t *free_bytes) {
    char a[320];
    struct statvfs sv;
    if (!full_path((pfs_t *)vfs, "/", a, sizeof(a))) return NEMA_ERR_RANGE;
    if (statvfs(a, &sv) != 0) return map_errno(errno);
    if (total) *total = (uint64_t)sv.f_blocks * sv.f_frsize;
    if (free_bytes) *free_bytes = (uint64_t)sv.f_bavail * sv.f_frsize;
    return NEMA_OK;
}

static const nema_fs_ops_t POSIXFS = {"posixfs", p_open, p_stat, p_readdir, p_mkdir, p_unlink, p_rename, p_statfs};
const nema_fs_ops_t *nema_posixfs_ops(void) { return &POSIXFS; }

nema_err_t nema_posixfs_mkdir(const char *host_dir) {
    if (mkdir(host_dir, 0777) == 0 || errno == EEXIST) return NEMA_OK;
    return map_errno(errno);
}

void *nema_posixfs_create(const char *root) {
    nema_owner_t owner = nema_owner_register("posixfs");
    if (strlen(root) >= sizeof(((pfs_t *)0)->root)) return NULL; /* never silently truncate a root path */
    pfs_t *fs = (pfs_t *)nema_calloc(NEMA_MEM_ANY, owner, sizeof(*fs));
    if (!fs) return NULL;
    nema_strlcpy(fs->root, root, sizeof(fs->root));
    size_t l = strlen(fs->root);
    if (l > 1 && fs->root[l - 1] == '/') fs->root[l - 1] = 0;
    fs->owner = owner;
    return fs;
}
