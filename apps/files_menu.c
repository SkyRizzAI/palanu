/* The Files operations menu (view "filemenu", an opaque overlay with the status bar): the reference FileOpsModal list look
 *. A ListSection with the entry's name, then the rows: a list with a
 * 2 px inset, the selected row inverted, a scrollbar when the rows do not fit. Up and Down move (no wrap), Enter picks, Esc
 * closes. Drawing goes through kui (logical 1-bit canvas, 2 px per pixel). It describes itself as a picker (pal/shell.h), so a session
 * that draws the shell services (Athena) shows it in its own look. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "files_menu.h"
#include "libapp.h"
#include "pal/desktop.h"
#include "pal/kui.h"
#include "pal/overlay.h"
#include "pal/shell.h"

#define ROW_PAD 2   /* the list's inset from the header and the sides */
#define BAR_X (k->w - 3) /* the scrollbar's x (logical): the 3 px bar at the right edge, at any UI scale */
#define NAME_MAX 40 /* the title kept (the file name, shortened by the caller when long) */

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full);
static bool key(pal_view_t *v, const pal_key_t *k);
static void leave(pal_view_t *v);
static bool describe(pal_view_t *v, pal_shell_view_t *o);

static struct {
    pal_view_t view;
    bool used, done;
    char title[NAME_MAX];
    char text[FILES_MENU_ITEMS][24];
    const char *items[FILES_MENU_ITEMS];
    int n, sel, top, vis;
    files_menu_cb_t cb;
    void *ctx;
} M = {.view = {.id = "filemenu", .title = "Options", .flags = PAL_VIEW_OPAQUE | PAL_VIEW_HIDDEN, .draw = draw, .key = key, .leave = leave,
                 .describe = describe}};
static kui_canvas_t M_KC NEMA_EXT_BSS; /* the logical canvas, in external RAM where the board has it */

static void finish(int index) {
    if (M.done) return;
    M.done = true;
    files_menu_cb_t cb = M.cb;
    void *ctx = M.ctx;
    M.used = false;
    pal_desktop_overlay_remove(&M.view);
    if (cb) cb(index, ctx);
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&M_KC, c->w, c->h, kui_scale_q());
    kui_canvas_t *k = &M_KC;
    int ay = kui_lg(k, a.y), ah = kui_lg(k, a.y + a.h) - ay;
    kui_clip(k, 0, ay, k->w, ah);
    int y = ay + kui_section(k, 0, ay, &FONT_BOLD8, M.title);
    int top = y + ROW_PAD, list_h = ay + ah - top - ROW_PAD;
    int vis = (list_h + 2) / KUI_LIST_PITCH; /* the last row's gap may be cut by the bottom edge */
    if (vis < 1) vis = 1;
    M.vis = vis;
    if (M.sel < M.top) M.top = M.sel;
    if (M.sel >= M.top + vis) M.top = M.sel - vis + 1;
    if (M.top < 0) M.top = 0;
    bool bar = M.n > vis;
    int rw = k->w - 2 * ROW_PAD - (bar ? 3 : 0); /* The reference SCROLLBAR_BAR_INSET */
    for (int i = M.top; i < M.n; i++) {
        int ry = top + (i - M.top) * KUI_LIST_PITCH;
        if (ry >= ay + ah) break;
        kui_list_item_t it = {0};
        it.label = M.items[i];
        it.icon = -1;
        it.selected = i == M.sel;
        kui_list_item(k, ROW_PAD, ry, rw, &it);
    }
    if (bar) kui_scrollbar(k, BAR_X, y, ay + ah - y, M.top * KUI_LIST_PITCH, list_h, M.n * KUI_LIST_ROW_H + (M.n - 1) * 2);
    kui_present(c, k, a);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    switch (k->sym) {
    case PAL_KEY_UP:
        if (M.sel > 0) M.sel--;
        break;
    case PAL_KEY_DOWN:
        if (M.sel + 1 < M.n) M.sel++;
        break;
    case PAL_KEY_ENTER: finish(M.sel); return true;
    case PAL_KEY_ESC: finish(-1); return true;
    default: return true;
    }
    pal_desktop_invalidate();
    return true;
}

static bool describe(pal_view_t *v, pal_shell_view_t *o) {
    (void)v;
    if (!M.used) return false;
    o->kind = PAL_SHELL_PICKER;
    o->title = M.title;
    o->nitems = M.n > PAL_SHELL_ITEMS ? PAL_SHELL_ITEMS : M.n;
    for (int i = 0; i < o->nitems; i++) o->items[i] = M.items[i];
    o->sel = M.sel;
    return true;
}

static void leave(pal_view_t *v) {
    (void)v;
    finish(-1); /* taken away by the desktop: the callback still runs once */
}

nema_err_t files_menu_open(const char *title, const char *const *items, int n, files_menu_cb_t cb, void *ctx) {
    if (!title || !items || n < 1 || n > FILES_MENU_ITEMS) return NEMA_ERR_INVAL;
    if (M.used) return NEMA_ERR_BUSY;
    M.used = true;
    M.done = false;
    snprintf(M.title, sizeof(M.title), "%s", title);
    for (int i = 0; i < n; i++) {
        snprintf(M.text[i], sizeof(M.text[i]), "%s", items[i] ? items[i] : "");
        M.items[i] = M.text[i];
    }
    M.n = n;
    M.sel = 0;
    M.top = 0;
    M.cb = cb;
    M.ctx = ctx;
    nema_err_t r = pal_desktop_overlay_push(&M.view);
    if (r != NEMA_OK) {
        M.used = false;
        M.done = true;
    }
    return r;
}
