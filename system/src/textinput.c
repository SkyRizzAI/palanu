/* PalanuOS text input: a single-line editor for the physical keyboard. Characters insert at the caret, Backspace and
 * Delete remove, Left, Right, Home and End move, Enter is OK, Esc cancels. The text scrolls horizontally when it is wider
 * than the field; masked mode shows '*' (passwords). One editor at a time: a second open gets NEMA_ERR_BUSY. */
#include <string.h>

#include "desktop_int.h"
#include "dialog_int.h"
#include "nema/nema.h"
#include "pal/dialog.h"
#include "pal/display.h"
#include "pal/overlay.h"
#include "pal/shell.h"
#include "pal/theme.h"
#include "theme_widgets.h"

#define BLINK_MS 500
#define BOX_MAX_W 280

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full);
static bool key(pal_view_t *v, const pal_key_t *k);
static void tick(pal_view_t *v, uint32_t now);
static void leave(pal_view_t *v);
static bool describe(pal_view_t *v, pal_shell_view_t *o);

static const pal_legend_item_t LEGENDS[] = {{PAL_ACT_OK, "done", NULL}, {PAL_ACT_BACK, "cancel", NULL}};

static struct {
    pal_view_t view;
    bool used, done, mask, caret_on;
    char title[PAL_DIALOG_TITLE];
    char buf[PAL_DIALOG_TEXT];
    int len, caret, scroll, max;
    pal_textinput_cb_t cb;
    void *ctx;
} TI = {.view = {.id = "textinput", .title = "Text input", .draw = draw, .key = key, .leave = leave, .tick = tick,
                 .tick_ms = BLINK_MS, .data = &TI, .legends = LEGENDS, .nlegends = 2, .describe = describe}};

static void release(void) {
    desk_lock();
    TI.used = false;
    desk_unlock();
}

/* Characters that fit in the field, from the screen width (the editor is always full width). */
static int box_w(void) {
    int w = pal_display_canvas()->w - 24;
    return w > BOX_MAX_W ? BOX_MAX_W : w;
}

static int field_vis(const pal_theme_t *t) {
    int fieldw = box_w() - 2 * (t->tokens.pad_x + 4);
    int vis = (fieldw - 2 * t->tokens.pad_x) / t->tokens.body->w;
    return vis < 1 ? 1 : vis;
}

static void fix_scroll(const pal_theme_t *t) {
    int vis = field_vis(t);
    if (TI.caret < TI.scroll) TI.scroll = TI.caret;
    if (TI.caret - TI.scroll >= vis) TI.scroll = TI.caret - vis + 1;
    if (TI.scroll < 0) TI.scroll = 0;
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    const pal_font_t *fb = t->tokens.body, *ft = t->tokens.title;
    int pad = t->tokens.pad_x + 4;
    int W = box_w(), fieldw = W - 2 * pad, vis = field_vis(t);
    int strip = TI.title[0] ? ft->h + 8 : 0;
    int H = strip + pad + (fb->h + 6) + pad;
    int x = a.x + (a.w - W) / 2;
    int y = a.y + (a.h - H) / 2;
    if (y < a.y) y = a.y;
    pal_rect_t box = {x, y, W, H};
    pal_def_frame(c, t, box, TI.title);
    fix_scroll(t);
    char shown[PAL_DIALOG_TEXT];
    int n = 0;
    for (int i = TI.scroll; i < TI.len && n < vis; i++, n++) shown[n] = TI.mask ? '*' : TI.buf[i];
    shown[n] = 0;
    t->comp.text_field(c, t, x + pad, y + strip + pad, fieldw, shown, TI.caret - TI.scroll, TI.caret_on);
}

static void finish(bool ok) {
    if (TI.done) return;
    TI.done = true;
    char out[PAL_DIALOG_TEXT];
    if (ok) memcpy(out, TI.buf, (size_t)TI.len + 1);
    else out[0] = 0;
    pal_textinput_cb_t cb = TI.cb;
    void *ctx = TI.ctx;
    ovl_remove_now(&TI.view);
    release();
    if (cb) cb(ok, out, ctx);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    switch (k->sym) {
    case PAL_KEY_CHAR:
        if (!(k->mods & (PAL_MOD_CTRL | PAL_MOD_ALT)) && k->cp >= 0x20 && k->cp < 0x7f && TI.len < TI.max) {
            memmove(TI.buf + TI.caret + 1, TI.buf + TI.caret, (size_t)(TI.len - TI.caret + 1));
            TI.buf[TI.caret++] = (char)k->cp;
            TI.len++;
        }
        break;
    case PAL_KEY_BACKSPACE:
        if (TI.caret > 0) {
            memmove(TI.buf + TI.caret - 1, TI.buf + TI.caret, (size_t)(TI.len - TI.caret + 1));
            TI.caret--;
            TI.len--;
        }
        break;
    case PAL_KEY_DELETE:
        if (TI.caret < TI.len) {
            memmove(TI.buf + TI.caret, TI.buf + TI.caret + 1, (size_t)(TI.len - TI.caret));
            TI.len--;
        }
        break;
    case PAL_KEY_LEFT: if (TI.caret > 0) TI.caret--; break;
    case PAL_KEY_RIGHT: if (TI.caret < TI.len) TI.caret++; break;
    case PAL_KEY_HOME: TI.caret = 0; break;
    case PAL_KEY_END: TI.caret = TI.len; break;
    case PAL_KEY_ENTER: finish(true); return true;
    case PAL_KEY_ESC: finish(false); return true;
    default: break;
    }
    TI.caret_on = true;
    pal_desktop_invalidate();
    return true;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    TI.caret_on = !TI.caret_on;
    pal_desktop_invalidate();
}

/* Removed by someone else: the caller hears a cancel. */
/* The entry as shown: asterisks when masked. */
static char s_shown[PAL_DIALOG_TEXT] NEMA_EXT_BSS;
static bool describe(pal_view_t *v, pal_shell_view_t *o) {
    (void)v;
    for (int i = 0; i < TI.len; i++) s_shown[i] = TI.mask ? '*' : TI.buf[i];
    s_shown[TI.len] = 0;
    o->kind = PAL_SHELL_TEXT;
    o->title = TI.title;
    o->text = s_shown;
    o->cursor = TI.caret;
    return true;
}

static void leave(pal_view_t *v) {
    (void)v;
    if (TI.done) return;
    TI.done = true;
    pal_textinput_cb_t cb = TI.cb;
    void *ctx = TI.ctx;
    release();
    if (cb) cb(false, "", ctx);
}

static void post_push(void *arg) {
    (void)arg;
    if (ovl_push_now(&TI.view, 0) != NEMA_OK) {
        pal_textinput_cb_t cb = TI.cb;
        void *ctx = TI.ctx;
        release();
        if (cb) cb(false, "", ctx);
    }
}

nema_err_t pal_textinput_open(const char *title, const char *initial, int max_len, bool mask, pal_textinput_cb_t cb,
                              void *ctx) {
    if (!title || max_len < 1 || max_len > PAL_DIALOG_TEXT - 1) return NEMA_ERR_INVAL;
    desk_lock();
    if (TI.used) {
        desk_unlock();
        return NEMA_ERR_BUSY;
    }
    TI.used = true;
    TI.done = false;
    TI.mask = mask;
    TI.caret_on = true;
    TI.max = max_len;
    nema_strlcpy(TI.title, title, sizeof(TI.title));
    nema_strlcpy(TI.buf, initial ? initial : "", sizeof(TI.buf));
    if (TI.max < (int)strlen(TI.buf)) TI.buf[TI.max] = 0;
    TI.len = (int)strlen(TI.buf);
    TI.caret = TI.len;
    TI.scroll = 0;
    TI.cb = cb;
    TI.ctx = ctx;
    desk_unlock();
    if (!desk_started() || desk_on_task()) {
        nema_err_t r = ovl_push_now(&TI.view, 0);
        if (r != NEMA_OK) release();
        return r;
    }
    nema_err_t r = desk_post(post_push, NULL);
    if (r != NEMA_OK) release();
    return r;
}

static void wait_cb(bool ok, const char *text, void *ctx) {
    dlg_waiter_t *w = (dlg_waiter_t *)ctx;
    w->ok = ok;
    nema_strlcpy(w->text, text, sizeof(w->text));
    w->done = true;
    nema_notify(&w->obj);
}

bool pal_textinput_wait(const char *title, const char *initial, int max_len, bool mask, char *out, size_t cap) {
    dlg_waiter_t w;
    if (out && cap) out[0] = 0;
    if (!dlg_wait_begin(&w, "pal_textinput_wait")) return false;
    if (pal_textinput_open(title, initial, max_len, mask, wait_cb, &w) != NEMA_OK) return false;
    dlg_wait_end(&w);
    if (w.ok && out && cap) nema_strlcpy(out, w.text, cap);
    return w.ok;
}
