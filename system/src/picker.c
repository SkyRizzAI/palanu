/* PalanuOS picker: a list of items to choose from, in the reference list-style modal: a section
 * title and the rows, the focused row inverted, on a screen of its own (opaque, so the layers below are not drawn). Up and
 * Down (Fn+W/S) move with wrap-around, Enter picks (the callback gets the index), Esc cancels (-1). One picker at a time. The
 * item pointers are kept, not copied. */
#include <string.h>

#include "desktop_int.h"
#include "nema/nema.h"
#include "pal/dialog.h"
#include "pal/kui.h"
#include "pal/overlay.h"
#include "pal/shell.h"

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full);
static bool key(pal_view_t *v, const pal_key_t *k);
static void leave(pal_view_t *v);
static bool describe(pal_view_t *v, pal_shell_view_t *o);

static struct {
    pal_view_t view;
    bool used, done;
    char title[PAL_DIALOG_TITLE];
    const char *items[PAL_PICKER_ITEMS];
    int n, sel, top, vis;
    pal_picker_cb_t cb;
    void *ctx;
} PK = {.view = {.id = "picker", .title = "Picker", .flags = PAL_VIEW_OPAQUE, .draw = draw, .key = key, .leave = leave,
                 .data = &PK, .describe = describe}};

static kui_canvas_t S NEMA_EXT_BSS;

static void release(void) {
    desk_lock();
    PK.used = false;
    desk_unlock();
}

/* Rows that fit under the section title (the list's 2 px inset included), at least one. */
static int rows_fit(int y, int bottom) {
    int list_y = y + 16 + 2; /* the section's height and the list's inset */
    int n = (bottom - list_y) / KUI_LIST_PITCH;
    return n < 1 ? 1 : n;
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&S, c->w, c->h, kui_scale_q());
    const int ay = kui_lg(&S, a.y), bottom = kui_lg(&S, a.y + a.h);
    PK.vis = rows_fit(ay, bottom);
    if (PK.vis > PK.n) PK.vis = PK.n;
    kui_list_page(&S, 0, ay, S.w, bottom - ay, PK.title, PK.items, PK.n, PK.sel, PK.top);
    kui_present(c, &S, a);
}

static void finish(int index) {
    if (PK.done) return;
    PK.done = true;
    pal_picker_cb_t cb = PK.cb;
    void *ctx = PK.ctx;
    ovl_remove_now(&PK.view);
    release();
    if (cb) cb(index, ctx);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    switch (k->sym) {
    case PAL_KEY_UP:
    case PAL_KEY_LEFT: PK.sel = (PK.sel + PK.n - 1) % PK.n; break;
    case PAL_KEY_DOWN:
    case PAL_KEY_RIGHT: PK.sel = (PK.sel + 1) % PK.n; break;
    case PAL_KEY_ENTER: finish(PK.sel); return true;
    case PAL_KEY_ESC: finish(-1); return true;
    default: return true;
    }
    if (PK.sel < PK.top) PK.top = PK.sel;
    if (PK.vis > 0 && PK.sel >= PK.top + PK.vis) PK.top = PK.sel - PK.vis + 1;
    pal_desktop_invalidate();
    return true;
}

static bool describe(pal_view_t *v, pal_shell_view_t *o) {
    (void)v;
    o->kind = PAL_SHELL_PICKER;
    o->title = PK.title;
    o->nitems = PK.n < PAL_SHELL_ITEMS ? PK.n : PAL_SHELL_ITEMS;
    for (int i = 0; i < o->nitems; i++) o->items[i] = PK.items[i];
    o->sel = PK.sel;
    return true;
}

static void leave(pal_view_t *v) {
    (void)v;
    if (PK.done) return;
    PK.done = true;
    pal_picker_cb_t cb = PK.cb;
    void *ctx = PK.ctx;
    release();
    if (cb) cb(-1, ctx);
}

static void post_push(void *arg) {
    (void)arg;
    if (ovl_push_now(&PK.view, 0) != NEMA_OK) {
        pal_picker_cb_t cb = PK.cb;
        void *ctx = PK.ctx;
        release();
        if (cb) cb(-1, ctx);
    }
}

nema_err_t pal_picker_open(const char *title, const char *const *items, int n, pal_picker_cb_t cb, void *ctx) {
    if (!title || !items || n < 1 || n > PAL_PICKER_ITEMS) return NEMA_ERR_INVAL;
    desk_lock();
    if (PK.used) {
        desk_unlock();
        return NEMA_ERR_BUSY;
    }
    PK.used = true;
    PK.done = false;
    PK.n = n;
    PK.sel = 0;
    PK.top = 0;
    PK.vis = 1;
    nema_strlcpy(PK.title, title, sizeof(PK.title));
    for (int i = 0; i < n; i++) PK.items[i] = items[i] ? items[i] : "";
    PK.cb = cb;
    PK.ctx = ctx;
    desk_unlock();
    if (!desk_started() || desk_on_task()) {
        nema_err_t r = ovl_push_now(&PK.view, 0);
        if (r != NEMA_OK) release();
        return r;
    }
    nema_err_t r = desk_post(post_push, NULL);
    if (r != NEMA_OK) release();
    return r;
}
