/* The Files app's model: see files_model.h. The listing reads through nema_fs_readdir by index, so a directory is read a few
 * entries per tick and the input stays responsive; the array is PSRAM when the board has it (nema_alloc ANY). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "files_model.h"
#include "nema/nema.h"
#include "pal/sd.h"

static const char *const ROOT_PATHS[FILES_ROOTS] = {"/home", "/data", "/apps", "/tmp", "/sd"};

nema_err_t files_list_begin(files_list_t *L, const char *dir) {
    if (!L->ents) {
        L->ents = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, FILES_MAX * sizeof(files_ent_t));
        if (!L->ents) return NEMA_ERR_NOMEM;
    }
    snprintf(L->dir, sizeof(L->dir), "%s", dir);
    L->roots = strcmp(dir, "/") == 0;
    L->n = 0;
    L->next = 0;
    L->truncated = false;
    L->err = NEMA_OK;
    L->loading = !L->roots;
    if (L->roots) {
        const char *paths[FILES_ROOTS];
        int k = files_roots(paths);
        for (int i = 0; i < k; i++) {
            uint64_t total = 0, free_bytes = 0;
            files_ent_t *e = &L->ents[L->n++];
            snprintf(e->name, sizeof(e->name), "%s", paths[i]);
            e->type = FILES_T_ROOT;
            e->size = nema_fs_statfs(paths[i], &total, &free_bytes) == NEMA_OK ? (uint32_t)(free_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : free_bytes) : 0;
        }
    }
    return NEMA_OK;
}

void files_list_free(files_list_t *L) {
    nema_free(L->ents);
    L->ents = NULL;
    L->n = 0;
    L->loading = false;
}

int files_list_step(files_list_t *L, int max) {
    if (!L->loading || !L->ents) return 0;
    int read = 0;
    while (read < max) {
        nema_dirent_t d;
        nema_err_t r = nema_fs_readdir(L->dir, L->next, &d);
        if (r == NEMA_ERR_NOENT) {
            L->loading = false;
            break;
        }
        if (r != NEMA_OK) {
            L->err = r;
            L->loading = false;
            break;
        }
        L->next++;
        read++;
        if (L->n >= FILES_MAX) {
            L->truncated = true;
            continue;
        }
        files_ent_t *e = &L->ents[L->n++];
        snprintf(e->name, sizeof(e->name), "%s", d.name);
        e->type = d.type;
        e->size = d.size;
    }
    if (!L->loading) files_sort(L->ents, L->n);
    return read;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int files_cmp(const files_ent_t *a, const files_ent_t *b) {
    bool da = a->type != NEMA_T_FILE, db = b->type != NEMA_T_FILE;
    if (da != db) return da ? -1 : 1;
    for (const char *x = a->name, *y = b->name;; x++, y++) {
        int cx = lower((unsigned char)*x), cy = lower((unsigned char)*y);
        if (cx != cy) return cx - cy;
        if (!*x) break;
    }
    return strcmp(a->name, b->name);
}

static int cmp_thunk(const void *a, const void *b) { return files_cmp((const files_ent_t *)a, (const files_ent_t *)b); }

void files_sort(files_ent_t *e, int n) {
    if (n > 1) qsort(e, (size_t)n, sizeof(*e), cmp_thunk);
}

int files_find(const files_list_t *L, const char *name) {
    for (int i = 0; i < L->n; i++)
        if (strcmp(L->ents[i].name, name) == 0) return i;
    return -1;
}

void files_join(char *out, size_t cap, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, cap, "/%s", name);
    else snprintf(out, cap, "%s/%s", dir, name);
}

bool files_parent(char *out, size_t cap, const char *path) {
    if (strcmp(path, "/") == 0) return false;
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) snprintf(out, cap, "/");
    else snprintf(out, cap, "%.*s", (int)(slash - path), path);
    return true;
}

const char *files_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

bool files_under(const char *path, const char *root) {
    size_t n = strlen(root);
    if (strncmp(path, root, n) != 0) return false;
    if (strcmp(root, "/") == 0) return true;
    return path[n] == 0 || path[n] == '/';
}

void files_path_fit(char *out, size_t cap, const char *path, int maxchars) {
    size_t n = strlen(path);
    if (cap == 0) return;
    if ((int)n <= maxchars || maxchars < 4) {
        snprintf(out, cap, "%s", path);
        return;
    }
    size_t keep = (size_t)maxchars - 3;
    size_t start = n - keep;
    while (start < n && path[start] != '/') start++; /* begin at a segment boundary when there is one */
    if (start >= n) start = n - keep;
    snprintf(out, cap, "...%s", path + start);
}

/* One segment of the breadcrumb as the reference shows it: names over 8 characters become 7 and a '~'. */
static void crumb_add(char *tmp, size_t cap, size_t *w, const char *seg, size_t n) {
    char ab[16];
    if (n > 8) snprintf(ab, sizeof(ab), "%.7s~", seg);
    else snprintf(ab, sizeof(ab), "%.*s", (int)n, seg);
    int r = snprintf(tmp + *w, cap - *w, " > %s", ab);
    if (r > 0 && (size_t)r < cap - *w) *w += (size_t)r;
}

void files_crumb(char *out, size_t cap, const char *path) {
    if (cap == 0) return;
    if (strcmp(path, "/") == 0) {
        snprintf(out, cap, "/");
        return;
    }
    /* the segments of the path, at most 16 kept (the header has no room for more) */
    const char *seg[16];
    size_t len[16];
    int ns = 0;
    const char *p = path;
    while (*p && ns < 16) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        seg[ns] = p;
        len[ns] = n;
        ns++;
        p += n;
    }
    if (ns == 0) {
        snprintf(out, cap, "/");
        return;
    }
    char tmp[128];
    size_t w = 1;
    tmp[0] = '/';
    tmp[1] = 0;
    if (ns <= 3) {
        for (int i = 0; i < ns; i++) crumb_add(tmp, sizeof(tmp), &w, seg[i], len[i]);
    } else {
        /* first, "...", then the last two */
        crumb_add(tmp, sizeof(tmp), &w, seg[0], len[0]);
        int r = snprintf(tmp + w, sizeof(tmp) - w, " > ...");
        if (r > 0 && (size_t)r < sizeof(tmp) - w) w += (size_t)r;
        crumb_add(tmp, sizeof(tmp), &w, seg[ns - 2], len[ns - 2]);
        crumb_add(tmp, sizeof(tmp), &w, seg[ns - 1], len[ns - 1]);
    }
    if (w > 22) {
        char ab[16];
        if (len[ns - 1] > 8) snprintf(ab, sizeof(ab), "%.7s~", seg[ns - 1]);
        else snprintf(ab, sizeof(ab), "%.*s", (int)len[ns - 1], seg[ns - 1]);
        snprintf(out, cap, "... > %s", ab);
    } else {
        snprintf(out, cap, "%s", tmp);
    }
}

bool files_valid_name(const char *name) {
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (strlen(name) >= NEMA_NAME_MAX) return false;
    for (const char *p = name; *p; p++)
        if (*p == '/' || (unsigned char)*p < 0x20) return false;
    return true;
}

void files_size_str(char *out, size_t cap, uint64_t bytes) {
    if (bytes < 1024u) snprintf(out, cap, "%uB", (unsigned)bytes);
    else if (bytes < 1024u * 1024u) snprintf(out, cap, "%uK", (unsigned)(bytes / 1024u));
    else snprintf(out, cap, "%uM", (unsigned)(bytes / (1024u * 1024u)));
}

bool files_is_text_name(const char *name) {
    static const char *const EXT[] = {".txt", ".md", ".c", ".h", ".log", ".json", ".cfg", ".ini", ".sh", ".conf", ".csv",
                                      ".xml", ".js", ".ts", ".py", ".lua", ".esc", ".toml", ".yaml", ".yml", ".rst"};
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    for (size_t i = 0; i < sizeof(EXT) / sizeof(EXT[0]); i++) {
        const char *e = EXT[i];
        size_t n = strlen(e);
        if (strlen(dot) != n) continue;
        bool same = true;
        for (size_t k = 0; k < n && same; k++) same = lower((unsigned char)dot[k]) == e[k];
        if (same) return true;
    }
    return false;
}

int files_kind(const files_ent_t *e) {
    if (e->type == FILES_T_ROOT) return FILES_K_ROOT;
    if (e->type != NEMA_T_FILE) return FILES_K_DIR;
    const char *dot = strrchr(e->name, '.');
    if (dot && strcmp(dot, ".papp") == 0) return FILES_K_PACKAGE;
    if (files_is_text_name(e->name)) return FILES_K_TEXT;
    return FILES_K_OTHER;
}

const char *files_icon_name(int kind) {
    switch (kind) {
    case FILES_K_DIR:
    case FILES_K_ROOT: return "folder";
    case FILES_K_TEXT: return "text";
    case FILES_K_PACKAGE: return "package";
    default: return "file";
    }
}

int files_roots(const char *out[FILES_ROOTS]) {
    int k = 0;
    for (int i = 0; i < FILES_ROOTS; i++) {
        nema_stat_t st;
        /* the card only while it is mounted: after an eject its mount point is an empty RAM directory */
        if (strcmp(ROOT_PATHS[i], PAL_SD_PATH) == 0 && pal_sd_present() && !pal_sd_mounted()) continue;
        if (nema_fs_stat(ROOT_PATHS[i], &st) == NEMA_OK && st.type == NEMA_T_DIR) out[k++] = ROOT_PATHS[i];
    }
    return k;
}
