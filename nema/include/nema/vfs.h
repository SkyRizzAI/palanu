/* Nema VFS: one path namespace over mounted filesystems.
 *
 * Why: the shell, apps, and the link all need the same view of storage regardless of where bytes live
 * (RAM, flash, SD card, or the virtual /bin). Filesystems implement a small operation table; the kernel
 * resolves paths against the calling process's working directory and returns ordinary objects, so files
 * work with read/write/poll/dup like every other handle.
 */
#ifndef NEMA_VFS_H
#define NEMA_VFS_H

#include "nema/obj.h"

#define NEMA_PATH_MAX 128
#define NEMA_NAME_MAX 32

#define NEMA_O_READ 0x01u
#define NEMA_O_WRITE 0x02u
#define NEMA_O_CREATE 0x04u
#define NEMA_O_TRUNC 0x08u
#define NEMA_O_APPEND 0x10u

#define NEMA_T_FILE 1
#define NEMA_T_DIR 2

typedef struct {
    uint32_t size;
    uint8_t type;
    uint32_t mtime;
} nema_stat_t;

typedef struct {
    char name[NEMA_NAME_MAX];
    uint8_t type;
    uint32_t size;
} nema_dirent_t;

/* ioctl requests on file objects */
#define NEMA_FIOC_SEEK 0x6601u /* arg: nema_seek_t* ; returns the new position in .off */
#define NEMA_FIOC_SIZE 0x6602u /* arg: uint32_t* */
#define NEMA_FIOC_SYNC 0x6603u /* no argument: flush the file to its medium (a no-op in RAM) */
typedef struct {
    int32_t off;
    int whence; /* 0 set, 1 current, 2 end */
} nema_seek_t;

/* A filesystem driver. Paths handed to it are absolute within the mount: "/" is the mount root. */
typedef struct {
    const char *name;
    nema_err_t (*open)(void *fs, const char *path, uint32_t flags, nema_obj_t **out);
    nema_err_t (*stat)(void *fs, const char *path, nema_stat_t *st);
    nema_err_t (*readdir)(void *fs, const char *path, size_t index, nema_dirent_t *ent); /* NEMA_ERR_NOENT at the end */
    nema_err_t (*mkdir)(void *fs, const char *path);
    nema_err_t (*unlink)(void *fs, const char *path); /* a file or an empty directory */
    nema_err_t (*rename)(void *fs, const char *from, const char *to);
    /* Optional, appended for the Files app: total and free bytes of the medium. NULL = not known. */
    nema_err_t (*statfs)(void *fs, uint64_t *total, uint64_t *free_bytes);
} nema_fs_ops_t;

nema_err_t nema_vfs_mount(const char *mountpoint, const nema_fs_ops_t *ops, void *fs);
nema_err_t nema_vfs_unmount(const char *mountpoint);

/* Turn path into an absolute, normalised path ("." and ".." resolved) using the calling process's cwd. */
nema_err_t nema_path_resolve(const char *path, char *out, size_t cap);

nema_err_t nema_fs_open(const char *path, uint32_t flags, nema_obj_t **out); /* the caller owns one reference */
nema_err_t nema_fs_stat(const char *path, nema_stat_t *st);
nema_err_t nema_fs_readdir(const char *path, size_t index, nema_dirent_t *ent);
nema_err_t nema_fs_mkdir(const char *path);
nema_err_t nema_fs_unlink(const char *path);
nema_err_t nema_fs_rename(const char *from, const char *to);
/* Total and free bytes of the filesystem that holds path. NEMA_ERR_NOSYS when that filesystem does not report space. */
nema_err_t nema_fs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes);

int nema_open(const char *path, uint32_t flags); /* installs a handle in the calling process */
nema_err_t nema_set_cwd(const char *path);       /* the target must be a directory */

/* The in-memory filesystem (backed by PSRAM) and the virtual /bin that lists registered apps. */
const nema_fs_ops_t *nema_ramfs_ops(void);
void *nema_ramfs_create(size_t max_bytes); /* NULL on failure */
const nema_fs_ops_t *nema_binfs_ops(void);

/* A filesystem over the POSIX file API rooted at a directory (LittleFS on the board, a folder on the host).
 * The root directory must already exist. */
const nema_fs_ops_t *nema_posixfs_ops(void);
void *nema_posixfs_create(const char *root);
nema_err_t nema_posixfs_mkdir(const char *host_dir); /* creates a host folder; NEMA_OK if it already exists */

#endif
