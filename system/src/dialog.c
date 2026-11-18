/* PalanuOS confirmation dialog: an overlay with a title, a text and two buttons, driven by the physical keyboard. The
 * answer goes to a callback on the desktop task. A small pool of slots lets two dialogs be open at once; the slots are static,
 * so nothing is allocated. Also the waiter behind the blocking variants (dialog_int.h). */
#include <string.h>

#include "desktop_int.h"
#include "dialog_int.h"
#include "nema/log.h"
#include "nema/nema.h"
#include "pal/dialog.h"
#include "pal/overlay.h"
#include "pal/shell.h"
#include "pal/theme.h"

#define SLOTS 2

typedef struct {
    pal_view_t view;
    bool used, done;
    char title[PAL_DIALOG_TITLE], text[PAL_DIALOG_TEXT];
    char labels[2][PAL_DIALOG_LABEL]; /* [0] Cancel, on the left; [1] OK, on the right (the reference ConfirmModal order) */
    pal_dialog_cb_t cb;
    void *ctx;
    int sel; /* 0 Cancel (the focus starts here, so Enter alone cancels), 1 OK */
} dlg_t;

static void release(dlg_t *d) {
    desk_lock();
    d->used = false;
    desk_unlock();
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)full;
    dlg_t *d = (dlg_t *)v->data;
    const pal_theme_t *t = pal_theme_active();
    const char *b[2] = {d->labels[0], d->labels[1]};
    t->comp.dialog_box(c, t, a, d->title, d->text, b, 2, d->sel);
}

static void finish(dlg_t *d, bool ok) {
    if (d->done) return;
    d->done = true;
    pal_dialog_cb_t cb = d->cb;
    void *ctx = d->ctx;
    ovl_remove_now(&d->view);
    release(d);
    if (cb) cb(ok, ctx);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    dlg_t *d = (dlg_t *)v->data;
    switch (k->sym) {
    case PAL_KEY_LEFT:
    case PAL_KEY_UP: d->sel = 0; break;
    case PAL_KEY_RIGHT:
    case PAL_KEY_DOWN: d->sel = 1; break;
    case PAL_KEY_TAB: d->sel ^= 1; break;
    case PAL_KEY_ENTER: finish(d, d->sel == 1); break;
    case PAL_KEY_ESC: finish(d, false); break;
    default: break; /* a modal dialog takes every key */
    }
    return true;
}

/* Removed by someone else (a home key, a switch, the desktop going home): the caller hears a cancel. */
static void leave(pal_view_t *v) {
    dlg_t *d = (dlg_t *)v->data;
    if (d->done) return;
    d->done = true;
    pal_dialog_cb_t cb = d->cb;
    void *ctx = d->ctx;
    release(d);
    if (cb) cb(false, ctx);
}

static bool describe(pal_view_t *v, pal_shell_view_t *o) {
    dlg_t *d = (dlg_t *)v->data;
    o->kind = PAL_SHELL_DIALOG;
    o->title = d->title;
    o->text = d->text;
    o->items[0] = d->labels[0];
    o->items[1] = d->labels[1];
    o->nitems = 2;
    o->sel = d->sel;
    return true;
}

#define VIEW(i) {.view = {.id = "dialog", .title = "Dialog", .draw = draw, .key = key, .leave = leave, .data = &DL[i], .describe = describe}}
static dlg_t DL[SLOTS] = {VIEW(0), VIEW(1)};

static void post_push(void *arg) {
    dlg_t *d = (dlg_t *)arg;
    if (ovl_push_now(&d->view, 0) != NEMA_OK) {
        release(d);
        if (d->cb) d->cb(false, d->ctx);
    }
}

nema_err_t pal_dialog_confirm(const char *title, const char *text, const char *ok_label, const char *cancel_label,
                              pal_dialog_cb_t cb, void *ctx) {
    if (!title || !text) return NEMA_ERR_INVAL;
    dlg_t *d = NULL;
    desk_lock();
    for (int i = 0; i < SLOTS && !d; i++)
        if (!DL[i].used) d = &DL[i];
    if (d) {
        d->used = true;
        d->done = false;
        d->sel = 0;
        nema_strlcpy(d->title, title, sizeof(d->title));
        nema_strlcpy(d->text, text, sizeof(d->text));
        nema_strlcpy(d->labels[0], cancel_label ? cancel_label : "Cancel", sizeof(d->labels[0]));
        nema_strlcpy(d->labels[1], ok_label ? ok_label : "OK", sizeof(d->labels[1]));
        d->cb = cb;
        d->ctx = ctx;
    }
    desk_unlock();
    if (!d) return NEMA_ERR_FULL;
    nema_err_t r;
    if (!desk_started() || desk_on_task()) {
        r = ovl_push_now(&d->view, 0);
        if (r != NEMA_OK) release(d);
        return r;
    }
    r = desk_post(post_push, d);
    if (r != NEMA_OK) release(d);
    return r;
}

/* ---- blocking variant ---- */

static int w_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static int w_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static uint32_t w_poll(nema_obj_t *o, uint32_t e) {
    dlg_waiter_t *w = (dlg_waiter_t *)o;
    (void)e;
    return w->done ? NEMA_POLL_IN : 0;
}
static nema_err_t w_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t WAIT_OPS = {"dialog-wait", w_read, w_write, w_poll, w_ioctl, NULL};

bool dlg_wait_begin(dlg_waiter_t *w, const char *who) {
    if (desk_on_task()) {
        NEMA_LOGE("dialog", "%s: a blocking dialog must not run on the desktop task", who);
        return false;
    }
    if (!desk_started()) {
        NEMA_LOGE("dialog", "%s: the desktop is not running", who);
        return false;
    }
    w->done = false;
    w->ok = false;
    w->text[0] = 0;
    nema_obj_init(&w->obj, &WAIT_OPS);
    return true;
}

static bool wait_pred(void *ctx) { return ((dlg_waiter_t *)ctx)->done; }

void dlg_wait_end(dlg_waiter_t *w) {
    nema_obj_t *o = &w->obj;
    while (!w->done) {
        if (nema_wait_on(&o, 1, wait_pred, w, -1) == NEMA_ERR_INTR) break; /* a fatal signal: the task is ending */
    }
}

static void wait_cb(bool ok, void *ctx) {
    dlg_waiter_t *w = (dlg_waiter_t *)ctx;
    w->ok = ok;
    w->done = true;
    nema_notify(&w->obj);
}

bool pal_dialog_confirm_wait(const char *title, const char *text, const char *ok_label, const char *cancel_label) {
    dlg_waiter_t w;
    if (!dlg_wait_begin(&w, "pal_dialog_confirm_wait")) return false;
    if (pal_dialog_confirm(title, text, ok_label, cancel_label, wait_cb, &w) != NEMA_OK) return false;
    dlg_wait_end(&w);
    return w.ok;
}

