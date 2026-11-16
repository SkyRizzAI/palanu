/* The Files app's model (no drawing, no desktop): the directory listing that is read a few entries per tick, the sort order,
 * path helpers, and the labels the rows use. The view (files.c) and the operations (files_ops.c) build on it. Tests:
 * tests/unit/test_files.c. */
#ifndef FILES_MODEL_H
#define FILES_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/vfs.h"

#define FILES_MAX 512      /* entries kept for one directory; more are counted as truncated */
#define FILES_T_ROOT 3     /* a virtual root in the "/" listing (size = its free bytes) */
#define FILES_ROOTS 5      /* /home, /data, /apps, /tmp, /sd */

typedef struct {
    char name[NEMA_NAME_MAX];
    uint8_t type; /* NEMA_T_FILE, NEMA_T_DIR or FILES_T_ROOT */
    uint32_t size;
} files_ent_t;

typedef struct {
    char dir[NEMA_PATH_MAX];
    bool roots;      /* the "/" listing: the roots, not a directory on disk */
    bool loading;    /* entries still being read */
    bool truncated;  /* the directory has more than FILES_MAX entries */
    nema_err_t err;  /* NEMA_OK, or why the directory could not be read */
    size_t next;     /* readdir index of the next entry */
    files_ent_t *ents; /* FILES_MAX entries, allocated on first use (memory owner: kernel) */
    int n;
} files_list_t;

/* Icon classes for a row. */
enum { FILES_K_DIR, FILES_K_TEXT, FILES_K_PACKAGE, FILES_K_OTHER, FILES_K_ROOT };

/* Start reading dir. "/" starts the roots listing (complete at once). Returns NEMA_ERR_NOMEM when the array cannot be made. */
nema_err_t files_list_begin(files_list_t *L, const char *dir);
void files_list_free(files_list_t *L);
/* Reads up to max entries (a tick's share). Returns the entries read by this call; loading turns false at the end. */
int files_list_step(files_list_t *L, int max);
/* Folders first, then case-insensitive name order; ties by exact name. */
void files_sort(files_ent_t *e, int n);
int files_cmp(const files_ent_t *a, const files_ent_t *b);
/* Index of the entry called name, or -1. */
int files_find(const files_list_t *L, const char *name);

/* Paths. dir "/" joined with a name gives "/name". */
void files_join(char *out, size_t cap, const char *dir, const char *name);
/* The parent of an absolute path; false for "/". */
bool files_parent(char *out, size_t cap, const char *path);
const char *files_base(const char *path);
/* True when path is root or lies inside it. */
bool files_under(const char *path, const char *root);
/* Shortens a path from the left to at most maxchars characters: "...ocs/notes/a.txt". */
void files_path_fit(char *out, size_t cap, const char *path, int maxchars);
/* The reference breadcrumb for the header: "/", "/ > home > fixture"; more than three segments show "/ > home > ... > leaf", and
   names over 8 characters end in '~'. A breadcrumb longer than 22 characters shows only "... > leaf". */
void files_crumb(char *out, size_t cap, const char *path);
/* A name is usable as one path segment: not empty, not "." or "..", no '/', no control characters. */
bool files_valid_name(const char *name);

/* Sizes: "512B", "12K", "3M" (the unit is the largest that fits; no decimals). */
void files_size_str(char *out, size_t cap, uint64_t bytes);
/* Icon class for an entry, from its type and name extension. */
int files_kind(const files_ent_t *e);
/* The 16 px icon name for a class ("folder", "text", "package", "file"). */
const char *files_icon_name(int kind);
/* True when the name suggests text (extensions such as .txt, .md, .c, .log, .json). Unknown names count as not text. */
bool files_is_text_name(const char *name);

/* The roots that exist on this board, in order, as absolute paths. Returns how many are mounted. */
int files_roots(const char *out[FILES_ROOTS]);

#endif
