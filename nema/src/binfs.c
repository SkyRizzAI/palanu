#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "nema/vfs.h"

/* /bin: a read-only directory that lists the registered apps. Apps are launched by name through the
 * registry; the entries exist so listing, completion, and `which` behave like a normal filesystem. */

typedef struct {
    nema_obj_t obj;
    char text[NEMA_NAME_MAX + 24];
    size_t len, pos;
} stub_t;

static int s_read(nema_obj_t *o, void *buf, size_t n) {
    stub_t *s = (stub_t *)o;
    size_t take = s->len - s->pos < n ? s->len - s->pos : n;
    memcpy(buf, s->text + s->pos, take);
    s->pos += take;
    return (int)take;
}
static int s_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static uint32_t s_poll(nema_obj_t *o, uint32_t e) { (void)o, (void)e; return NEMA_POLL_IN; }
static nema_err_t s_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static void s_destroy(nema_obj_t *o) { nema_free(o); }
static const nema_obj_ops_t STUB_OPS = {"binfile", s_read, s_write, s_poll, s_ioctl, s_destroy};

static const nema_app_t *app_at_visible(size_t index) {
    size_t seen = 0;
    for (size_t i = 0; i < nema_app_count(); i++) {
        const nema_app_t *a = nema_app_at(i);
        if (a->flags & NEMA_APP_HIDDEN) continue;
        if (seen++ == index) return a;
    }
    return NULL;
}

static nema_err_t b_open(void *fs, const char *path, uint32_t flags, nema_obj_t **out) {
    (void)fs;
    if (flags & (NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC)) return NEMA_ERR_PERM;
    const char *name = path + 1;
    if (!*name) return NEMA_ERR_ISDIR;
    const nema_app_t *a = nema_app_find(name);
    if (!a) return NEMA_ERR_NOENT;
    stub_t *s = (stub_t *)nema_calloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*s));
    if (!s) return NEMA_ERR_NOMEM;
    nema_obj_init(&s->obj, &STUB_OPS);
    s->len = (size_t)snprintf(s->text, sizeof(s->text), "#!palanu %s\n", a->name);
    *out = &s->obj;
    return NEMA_OK;
}

static nema_err_t b_stat(void *fs, const char *path, nema_stat_t *st) {
    (void)fs;
    memset(st, 0, sizeof(*st));
    if (path[1] == 0) {
        st->type = NEMA_T_DIR;
        return NEMA_OK;
    }
    if (!nema_app_find(path + 1)) return NEMA_ERR_NOENT;
    st->type = NEMA_T_FILE;
    return NEMA_OK;
}

static nema_err_t b_readdir(void *fs, const char *path, size_t index, nema_dirent_t *ent) {
    (void)fs;
    if (path[1] != 0) return NEMA_ERR_NOTDIR;
    const nema_app_t *a = app_at_visible(index);
    if (!a) return NEMA_ERR_NOENT;
    nema_strlcpy(ent->name, a->name, sizeof(ent->name));
    ent->type = NEMA_T_FILE;
    ent->size = 0;
    return NEMA_OK;
}

static const nema_fs_ops_t BINFS = {"binfs", b_open, b_stat, b_readdir, NULL, NULL, NULL, NULL};
const nema_fs_ops_t *nema_binfs_ops(void) { return &BINFS; }
