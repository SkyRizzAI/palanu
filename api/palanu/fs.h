/* Files, in the app's own namespace. An app sees /data (its persistent directory, kept across runs), /tmp (private to
 * one run, removed when the app ends), /home (needs fs.home) and /sd (needs fs.media). Any other path is refused with
 * PAL_ERR_PERM, and ".." cannot climb out of the namespace. A relative path starts in /data. Legacy manifests (API level
 * 1) with the "fs" permission also get /home. A handle the app opens is closed by the system when the app ends, so an
 * app that forgets close does not leak it. */
#ifndef PALANU_API_FS_H
#define PALANU_API_FS_H

#include "annot.h"

#define PAL_O_READ 0x01
#define PAL_O_WRITE 0x02
#define PAL_O_CREATE 0x04
#define PAL_O_TRUNC 0x08
#define PAL_O_APPEND 0x10

/* Error codes: negative values are errors (same numbers as the system). */
#define PAL_ERR_NOMEM (-1)
#define PAL_ERR_INVAL (-2)
#define PAL_ERR_NOENT (-3)
#define PAL_ERR_BUSY (-4)
#define PAL_ERR_IO (-12)
#define PAL_ERR_RANGE (-13)
#define PAL_ERR_BADF (-14)
#define PAL_ERR_FULL (-15)
#define PAL_ERR_NOSPC (-17)
#define PAL_ERR_ISDIR (-18)
#define PAL_ERR_NOTDIR (-19)
#define PAL_ERR_EXIST (-11)

/* Kinds in a stat or directory record (flags). */
#define PAL_FILE 1
#define PAL_DIR 2
/* Record sizes in bytes (see pal_stat and pal_dir_next). */
#define PAL_STAT_SIZE 16
#define PAL_DIRENT_SIZE 64
#define PAL_DIRENT_NAME 56
/* whence for pal_seek */
#define PAL_SEEK_SET 0
#define PAL_SEEK_CUR 1
#define PAL_SEEK_END 2

PAL_SINCE(1) PAL_CAP(fs) PAL_WASM(open)
PAL_DOC("Opens the file at path (len bytes, not NUL-terminated) with PAL_O_ flags. Returns a handle, or a negative error.")
int32_t pal_open(PAL_IN(len) const char *path, uint32_t len, int32_t flags);

PAL_SINCE(1) PAL_WASM(close)
PAL_DOC("Closes a handle that this app opened. Returns 0, or a negative error.")
int32_t pal_close(int32_t fd);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(stat)
PAL_DOC("Writes the 16-byte record of path into st_out: uint32 size, uint32 mtime in seconds since boot, uint32 flags (PAL_FILE or PAL_DIR), uint32 reserved; little-endian. Returns 0, or a negative error.")
int32_t pal_stat(const char *path, PAL_OUT(len) void *st_out, uint32_t len);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(dir_open)
PAL_DOC("Opens the directory at path for pal_dir_next. Returns a handle, or a negative error. Close it with pal_close.")
int32_t pal_dir_open(const char *path);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(dir_next)
PAL_DOC("Writes the next entry of directory handle dh into ent_out: uint32 size, uint32 flags (PAL_FILE or PAL_DIR), then the NUL-terminated name in 56 bytes. Returns 1 for an entry, 0 at the end, or a negative error.")
int32_t pal_dir_next(int32_t dh, PAL_OUT(len) void *ent_out, uint32_t len);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(seek)
PAL_DOC("Moves the position of file handle h: whence is PAL_SEEK_SET, PAL_SEEK_CUR or PAL_SEEK_END. Returns the new position, or a negative error.")
int64_t pal_seek(int32_t h, int64_t off, int32_t whence);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(mkdir)
PAL_DOC("Creates the directory at path. The parent must exist. Returns 0, or a negative error.")
int32_t pal_mkdir(const char *path);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(remove)
PAL_DOC("Removes the file, or the empty directory, at path. Returns 0, or a negative error.")
int32_t pal_remove(const char *path);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(rename)
PAL_DOC("Renames (moves) from to to, both inside the app's namespace. Returns 0, or a negative error.")
int32_t pal_rename(const char *from, const char *to);

PAL_SINCE(2) PAL_CAP(fs) PAL_WASM(fsync)
PAL_DOC("Flushes the file of handle h to its storage. Returns 0, or a negative error.")
int32_t pal_fsync(int32_t h);

#endif
