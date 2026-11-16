/* The on-screen keyboard: the controller (see pal/vk.h and docs/VKEYBOARD.md). State changes happen on the desktop task;
 * other tasks ask through a one-slot mailbox that the view's pump applies. */
#include <stdio.h>
#include <string.h>

#include "pal/desktop.h"
#include "pal/input.h"
#include "pal/overlay.h"
#include "pal/vk.h"
#include "vk_int.h"

#define FAST_MS 300      /* after a key, poll the source every tick for this long (the shell echoes asynchronously) */
#define POLL_MS 250      /* otherwise poll every 250 ms */
#define FLASH_MS 120
#define CARET_MS 500     /* blink period, once the user has been idle for IDLE_MS */
#define IDLE_MS 1000
#define TICK_MS 50

enum { REQ_NONE, REQ_OPEN, REQ_OPEN_TEXT, REQ_CLOSE, REQ_ENTRY, REQ_CANCEL };

vk_state_t vk_S = {.flash = -1};

static void complete(int len);
static void close_now(void);

/* ---- the view ---- */
static void nop_enter(pal_view_t *v) { (void)v; }
static void leave(pal_view_t *v);
static void pump(pal_view_t *v);
static void tick(pal_view_t *v, uint32_t now);
static bool key(pal_view_t *v, const pal_key_t *k);
static void draw_view(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v;
    vk_draw(c, a, full);
}

pal_view_t pal_vk_view = {
    .id = "vk",
    .title = "Keyboard",
    .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_OPAQUE | PAL_VIEW_HIDDEN, /* the status bar stays on top, as in the reference */
    .enter = nop_enter,
    .leave = leave,
    .draw = draw_view,
    .key = key,
    .pump = pump,
    .tick = tick,
    .tick_ms = TICK_MS,
};

/* ---- desktop and test seams ---- */
static uint32_t now_ms(void) { return vk_S.hooks.clock ? vk_S.hooks.clock() : pal_input_real_clock(); }
static void wake(void) {
    if (vk_S.hooks.wake) vk_S.hooks.wake();
    else pal_desktop_invalidate();
}
static nema_err_t do_push(void) { return vk_S.hooks.push ? vk_S.hooks.push(&pal_vk_view) : pal_desktop_overlay_push(&pal_vk_view); }
static void do_pop(void) {
    if (vk_S.hooks.pop) vk_S.hooks.pop(&pal_vk_view);
    else pal_desktop_overlay_remove(&pal_vk_view);
}
static bool present(void) { return vk_S.hooks.present ? vk_S.hooks.present() : pal_desktop_view("vk") != NULL; }
static void ensure_lock(void) {
    if (!vk_S.lock) vk_S.lock = nema_port_mutex_create();
}

void pal_vk_set_hooks(const pal_vk_hooks_t *h) {
    if (h) vk_S.hooks = *h;
    else memset(&vk_S.hooks, 0, sizeof(vk_S.hooks));
}

void pal_vk_register(void) {
    ensure_lock();
    pal_desktop_register(&pal_vk_view);
}

nema_err_t pal_vk_register_source(const char *view_id, pal_vk_source_t *src) {
    if (!view_id || !src) return NEMA_ERR_INVAL;
    for (int i = 0; i < VK_SOURCES; i++)
        if (vk_S.src_id[i] && strcmp(vk_S.src_id[i], view_id) == 0) {
            vk_S.src[i] = src;
            return NEMA_OK;
        }
    for (int i = 0; i < VK_SOURCES; i++)
        if (!vk_S.src_id[i]) {
            vk_S.src_id[i] = view_id;
            vk_S.src[i] = src;
            return NEMA_OK;
        }
    return NEMA_ERR_FULL;
}

static pal_vk_source_t *source_for(const char *id) {
    for (int i = 0; i < VK_SOURCES; i++)
        if (vk_S.src_id[i] && strcmp(vk_S.src_id[i], id) == 0) return vk_S.src[i];
    return NULL;
}

/* ---- the box ---- */
static void box_set(const char *s, size_t len, size_t cur) {
    if (len > VK_TEXT_MAX) len = VK_TEXT_MAX;
    if (cur > len) cur = len;
    if (len == vk_S.len && cur == vk_S.cursor && memcmp(vk_S.text, s, len) == 0) return;
    memcpy(vk_S.text, s, len);
    vk_S.text[len] = 0;
    vk_S.len = len;
    vk_S.cursor = cur;
    vk_S.dirty_text = true;
}

/* The box shows the line the front app is editing, or what was typed in this session when no line is edited. */
static void sync_box(void) {
    if (vk_S.fixed || vk_S.ui.entry) return;
    char buf[VK_TEXT_MAX + 1];
    size_t cur = 0;
    pal_vk_source_t *src = source_for(pal_desktop_front_id());
    int r = -1;
    if (src && src->get_text) r = src->get_text(src->ctx, buf, sizeof(buf), &cur);
    if (r >= 0) box_set(buf, (size_t)r, cur);
    else box_set(vk_S.sess, vk_S.sess_len, vk_S.sess_len);
}

static void set_title(const char *t) {
    snprintf(vk_S.title, sizeof(vk_S.title), "%s", t ? t : "Text");
}

static void front_title(void) {
    pal_view_t *v = pal_desktop_view(pal_desktop_front_id());
    set_title(v ? v->title : NULL);
}

/* ---- dirty marks ---- */
static void mark_key(int i) {
    if (i >= 0 && i < vk_S.ncells) vk_S.dirty_keys[i] = true;
}
static void mark_all_keys(void) {
    for (int i = 0; i < vk_S.ncells; i++) vk_S.dirty_keys[i] = true;
}
static void touch(void) {
    uint32_t t = now_ms();
    vk_S.now = t;
    vk_S.last_key_ms = t;
    vk_S.caret_on = true;
    vk_S.caret_at = t;
    vk_S.dirty_caret = true;
}

static void rebuild(void) {
    int n = vk_build(&vk_S.ui, vk_S.cells, VK_MAX_CELLS);
    vk_S.ncells = n < 0 ? 0 : n;
    vk_S.focus = vk_focus_home(vk_S.cells, vk_S.ncells, vk_S.ui.entry != 0);
    vk_S.pref2 = vk_S.ncells ? vk_pref2(vk_S.cells, vk_S.focus) : 0;
    vk_S.need_full = true;
}

static void set_layer(int layer) {
    vk_S.ui.layer = (uint8_t)layer;
    rebuild();
}

static void move_focus(vk_dir_t d) {
    vk_S.typed = false;
    int old = vk_S.focus;
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, d, &vk_S.pref2);
    mark_key(old);
    mark_key(vk_S.focus);
    touch();
}

static void flash_cell(int idx) {
    vk_S.flash = idx;
    vk_S.flash_until = now_ms() + FLASH_MS;
    mark_key(idx);
}

/* ---- sending ---- */
static void deliver(const pal_key_t *k) {
    if (vk_S.hooks.inject) {
        vk_S.hooks.inject(k);
        return;
    }
    pal_view_t *v = pal_desktop_view(pal_desktop_front_id());
    if (v && v->key) v->key(v, k);
}

static void sess_insert(uint32_t cp) { vk_S.sess_len = vk_text_insert(vk_S.sess, vk_S.sess_len, VK_TEXT_MAX + 1, cp); }
static void sess_back(void) { vk_S.sess_len = vk_text_backspace(vk_S.sess, vk_S.sess_len); }

/* A key the app receives (live mode). The session text follows what the app got; the box follows the app's line. */
static void live_send(const pal_key_t *k) {
    deliver(k);
    if (k->sym == PAL_KEY_ENTER) { /* Enter runs the line and ends the typing session: the keyboard closes, as OK does on a Switch */
        vk_S.sess_len = 0;
        close_now();
        return;
    }
    bool ctl = (k->mods & (PAL_MOD_CTRL | PAL_MOD_ALT)) != 0;
    if (k->sym == PAL_KEY_CHAR && !ctl) sess_insert(k->cp);
    else if (k->sym == PAL_KEY_BACKSPACE) sess_back();
    else if (k->sym == PAL_KEY_ENTER) vk_S.sess_len = 0;
    if (!source_for(pal_desktop_front_id())) sync_box();
    vk_S.fast_until = now_ms() + FAST_MS;
}

static void entry_insert(uint32_t cp) {
    vk_S.len = vk_text_insert(vk_S.text, vk_S.len, vk_S.max + 1, cp);
    vk_S.cursor = vk_S.len;
    vk_S.dirty_text = true;
}

static void entry_backspace(void) {
    vk_S.len = vk_text_backspace(vk_S.text, vk_S.len);
    vk_S.cursor = vk_S.len;
    vk_S.dirty_text = true;
}

static void confirm(void) {
    if (!vk_S.ui.entry) return;
    if (vk_S.len == 0 && !vk_S.allow_empty) {
        vk_S.dirty_caret = true; /* nothing to send: the caret says the box is empty */
        return;
    }
    complete((int)vk_S.len);
    close_now();
}

static void cancel(void) {
    complete(PAL_VK_CANCEL);
    close_now();
}

/* A key on the keyboard was pressed (on screen or by Enter on the physical keyboard). */
static void press(int idx) {
    if (idx < 0 || idx >= vk_S.ncells) return;
    touch();
    flash_cell(idx);
    const vk_key_t *k = vk_S.cells[idx].key;
    uint8_t phys = pal_input_mods_held();
    uint32_t now = vk_S.now;
    switch (k->kind) {
    case VK_K_LAYER: set_layer(k->layer); return;
    case VK_K_SHIFT:
        vk_shift_tap(&vk_S.ui, now);
        mark_all_keys();
        return;
    case VK_K_MOD:
        vk_S.ui.sticky ^= k->mod;
        mark_all_keys();
        return;
    case VK_K_SHOW:
        vk_S.ui.show = !vk_S.ui.show;
        vk_S.dirty_text = true;
        mark_key(idx);
        return;
    case VK_K_OK: confirm(); return;
    case VK_K_CANCEL:
        if (vk_S.ui.entry) cancel();
        return;
    default: break;
    }
    pal_key_t ev;
    if (!vk_key_event(k, vk_is_upper(&vk_S.ui, phys), &ev)) return;
    ev.mods |= (uint8_t)(vk_S.ui.sticky | phys);
    if (vk_S.ui.entry) {
        if (ev.sym == PAL_KEY_CHAR) entry_insert(ev.cp);
        else if (ev.sym == PAL_KEY_BACKSPACE) entry_backspace();
    } else {
        live_send(&ev);
    }
    if (k->kind == VK_K_CHAR && vk_S.ui.shift == VK_SHIFT_ONE) { /* the latched shift is for one letter */
        vk_S.ui.shift = VK_SHIFT_OFF;
        mark_all_keys();
    }
    if (vk_S.ui.sticky) { /* Ctrl and Alt are released after the key they were latched for */
        vk_S.ui.sticky = 0;
        mark_all_keys();
    }
}

/* ---- opening and closing ---- */
static void reset_ui(bool entry, bool mask) {
    memset(&vk_S.ui, 0, sizeof(vk_S.ui));
    vk_S.typed = false;
    vk_S.ui.entry = entry;
    vk_S.ui.mask = mask;
}

static void parse_layer(const char *name) {
    if (!name) return;
    if (strcmp(name, "ABC") == 0) {
        vk_S.ui.layer = VKL_LETTERS;
        vk_S.ui.shift = VK_SHIFT_ONE;
    } else if (strcmp(name, "123") == 0) vk_S.ui.layer = VKL_SYM;
    else if (strcmp(name, "#+=") == 0) vk_S.ui.layer = VKL_SYM2;
    else if (strcmp(name, "ctl") == 0) vk_S.ui.layer = VKL_CTL;
}

static nema_err_t open_live(const char *fixed_text, const char *layer) {
    if (vk_S.open) return NEMA_OK;
    reset_ui(false, false);
    vk_S.fixed = fixed_text != NULL;
    vk_S.max = VK_TEXT_MAX;
    vk_S.sess_len = 0;
    vk_S.allow_empty = true;
    vk_S.scroll = 0;
    vk_S.len = vk_S.cursor = 0;
    front_title();
    if (fixed_text) {
        box_set(fixed_text, strlen(fixed_text), strlen(fixed_text));
        parse_layer(layer);
    }
    rebuild();
    touch();
    vk_S.caret_on = true;
    vk_S.open = true;
    if (do_push() != NEMA_OK) {
        vk_S.open = false;
        return NEMA_ERR_FULL;
    }
    if (!fixed_text) sync_box();
    return NEMA_OK;
}

static void begin_entry(pal_vk_entry_t *e, void (*cb)(int, void *), void *ctx) {
    if (vk_S.open) {
        if (cb) cb(PAL_VK_CANCEL, ctx);
        return;
    }
    reset_ui(true, e->mask);
    vk_S.fixed = false;
    vk_S.max = e->cap > 1 ? e->cap - 1 : 0;
    if (vk_S.max > VK_TEXT_MAX) vk_S.max = VK_TEXT_MAX;
    vk_S.allow_empty = e->allow_empty;
    vk_S.len = 0;
    if (e->initial) {
        size_t n = strlen(e->initial);
        if (n > vk_S.max) n = vk_S.max;
        memcpy(vk_S.text, e->initial, n);
        vk_S.text[n] = 0;
        vk_S.len = n;
    } else {
        vk_S.text[0] = 0;
    }
    vk_S.cursor = vk_S.len;
    set_title(e->title);
    vk_S.ent = e; /* the buffer the result goes to; kept until complete() */
    vk_S.cb = cb;
    vk_S.ctx = ctx;
    rebuild();
    vk_S.focus = 0; /* The reference opens an entry on its first key, top left */
    vk_S.pref2 = vk_pref2(vk_S.cells, vk_S.focus);
    touch();
    vk_S.caret_on = true;
    vk_S.open = true;
    if (do_push() != NEMA_OK) {
        vk_S.open = false;
        complete(PAL_VK_CANCEL);
    }
}

/* Called when the entry ends (confirmed, cancelled, or the overlay went away): the result goes to the buffer, then to the
 * callback. Does not close the keyboard. */
static void complete(int len) {
    pal_vk_entry_t *e = vk_S.ent;
    void (*cb)(int, void *) = vk_S.cb;
    void *ctx = vk_S.ctx;
    vk_S.ent = NULL;
    vk_S.cb = NULL;
    vk_S.ctx = NULL;
    if (len >= 0 && e && e->buf && e->cap) {
        size_t n = vk_S.len < e->cap - 1 ? vk_S.len : e->cap - 1;
        memcpy(e->buf, vk_S.text, n);
        e->buf[n] = 0;
        len = (int)n;
    }
    if (e) { /* the entered text may be a password: do not keep a copy in the keyboard */
        memset(vk_S.text, 0, sizeof(vk_S.text));
        vk_S.len = vk_S.cursor = 0;
    }
    if (cb) cb(len, ctx);
}

static void close_now(void) {
    if (!vk_S.open) {
        if (vk_S.cb) complete(PAL_VK_CANCEL);
        return;
    }
    vk_S.open = false;
    vk_S.ui.entry = 0;
    do_pop();
    if (vk_S.cb) complete(PAL_VK_CANCEL);
}

static void leave(pal_view_t *v) {
    (void)v;
    vk_S.open = false;
    vk_S.ui.entry = 0;
    if (vk_S.cb) complete(PAL_VK_CANCEL); /* the overlay was removed by someone else: that is a cancel */
}

/* ---- mailbox ---- */
static nema_err_t post(int kind, const char *text, const char *layer, pal_vk_entry_t *e, void (*cb)(int, void *), void *ctx) {
    ensure_lock();
    if (!vk_S.lock) return NEMA_ERR_NOMEM;
    nema_port_mutex_lock(vk_S.lock);
    if (vk_S.req_pending || (kind == REQ_ENTRY && (vk_S.open || vk_S.cb))) {
        nema_port_mutex_unlock(vk_S.lock);
        return NEMA_ERR_BUSY;
    }
    vk_S.req_kind = kind;
    vk_S.req_text[0] = 0;
    if (text) snprintf(vk_S.req_text, sizeof(vk_S.req_text), "%s", text);
    vk_S.req_layer[0] = 0;
    if (layer) snprintf(vk_S.req_layer, sizeof(vk_S.req_layer), "%s", layer);
    vk_S.req_ent = e;
    vk_S.req_cb = cb;
    vk_S.req_ctx = ctx;
    vk_S.req_pending = true;
    nema_port_mutex_unlock(vk_S.lock);
    wake();
    return NEMA_OK;
}

static void pump(pal_view_t *v) {
    (void)v;
    vk_S.desktop_ctx = nema_tctx();
    if (!vk_S.lock) return;
    char text[VK_TEXT_MAX + 1], layer[8];
    int kind;
    pal_vk_entry_t *e;
    void (*cb)(int, void *);
    void *ctx;
    nema_port_mutex_lock(vk_S.lock);
    if (!vk_S.req_pending) {
        nema_port_mutex_unlock(vk_S.lock);
        return;
    }
    kind = vk_S.req_kind;
    memcpy(text, vk_S.req_text, sizeof(text));
    memcpy(layer, vk_S.req_layer, sizeof(layer));
    e = vk_S.req_ent;
    cb = vk_S.req_cb;
    ctx = vk_S.req_ctx;
    vk_S.req_pending = false;
    nema_port_mutex_unlock(vk_S.lock);
    switch (kind) {
    case REQ_OPEN: open_live(NULL, NULL); break;
    case REQ_OPEN_TEXT:
        close_now();
        open_live(text, layer);
        break;
    case REQ_CLOSE: close_now(); break;
    case REQ_ENTRY: begin_entry(e, cb, ctx); break;
    case REQ_CANCEL:
        if (vk_S.open) cancel();
        else if (vk_S.cb) complete(PAL_VK_CANCEL);
        break;
    default: break;
    }
    wake();
}

/* ---- public API ---- */
nema_err_t pal_vk_open(void) { return post(REQ_OPEN, NULL, NULL, NULL, NULL, NULL); }
void pal_vk_close(void) { (void)post(REQ_CLOSE, NULL, NULL, NULL, NULL, NULL); }
bool pal_vk_is_open(void) { return vk_S.open; }
nema_err_t pal_vk_open_text(const char *text, const char *layer) {
    return post(REQ_OPEN_TEXT, text ? text : "", layer, NULL, NULL, NULL);
}

void pal_vk_toggle(void) {
    if (vk_S.open) close_now();
    else open_live(NULL, NULL);
}

nema_err_t pal_vk_edit_async(pal_vk_entry_t *e, void (*cb)(int len, void *ctx), void *ctx) {
    if (!e || !e->buf || e->cap == 0) return NEMA_ERR_INVAL;
    if (!present()) return NEMA_ERR_NOSYS;
    return post(REQ_ENTRY, NULL, NULL, e, cb, ctx);
}

/* Blocking entry: waits on a notify object that the completion callback signals. */
static void blk_cb(int len, void *ctx) {
    (void)ctx;
    vk_S.blk_len = len;
    vk_S.blk_done = true;
    nema_notify(&vk_S.done);
}
static bool blk_pred(void *ctx) {
    (void)ctx;
    return vk_S.blk_done;
}
static uint32_t done_poll(nema_obj_t *o, uint32_t ev) {
    (void)o, (void)ev;
    return vk_S.blk_done ? NEMA_POLL_IN : 0;
}
static int done_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static int done_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static nema_err_t done_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t DONE_OPS = {"vk-entry", done_read, done_write, done_poll, done_ioctl, NULL};

int pal_vk_edit(pal_vk_entry_t *e) {
    if (!e || !e->buf || e->cap == 0) return NEMA_ERR_INVAL;
    if (!present()) return NEMA_ERR_NOSYS;
    if (vk_S.desktop_ctx && nema_tctx() == vk_S.desktop_ctx) return NEMA_ERR_PERM; /* the desktop must not block on itself */
    if (!vk_S.done_init) {
        nema_obj_init(&vk_S.done, &DONE_OPS);
        vk_S.done_init = true;
    }
    vk_S.blk_done = false;
    vk_S.blk_len = PAL_VK_CANCEL;
    nema_err_t r = post(REQ_ENTRY, NULL, NULL, e, blk_cb, NULL);
    if (r != NEMA_OK) return r;
    nema_obj_t *objs[1] = {&vk_S.done};
    bool cancel_sent = false;
    while (!vk_S.blk_done) {
        if (nema_wait_on(objs, 1, blk_pred, NULL, -1) != NEMA_OK && !cancel_sent) {
            /* a fatal signal (Ctrl-C of the command): close the keyboard; the entry then ends as a cancel */
            cancel_sent = true;
            (void)post(REQ_CANCEL, NULL, NULL, NULL, NULL, NULL);
        }
    }
    return vk_S.blk_len;
}

/* ---- keys ---- */
static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    vk_S.now = now_ms();
    switch (k->sym) {
    case PAL_KEY_VKB:
    case PAL_KEY_ESC:
        if (vk_S.ui.entry) cancel();
        else close_now();
        return true;
    case PAL_KEY_TAB: {
        vk_ui_t u = vk_S.ui;
        vk_cycle_layer(&u);
        set_layer(u.layer);
        touch();
        return true;
    }
    case PAL_KEY_UP: move_focus(VK_DIR_UP); return true;
    case PAL_KEY_DOWN: move_focus(VK_DIR_DOWN); return true;
    case PAL_KEY_LEFT: move_focus(VK_DIR_LEFT); return true;
    case PAL_KEY_RIGHT: move_focus(VK_DIR_RIGHT); return true;
    case PAL_KEY_HOME: move_focus(VK_DIR_HOME); return true;
    case PAL_KEY_END: move_focus(VK_DIR_END); return true;
    case PAL_KEY_PGUP: move_focus(VK_DIR_PGUP); return true;
    case PAL_KEY_PGDN: move_focus(VK_DIR_PGDN); return true;
    case PAL_KEY_ENTER:
        /* Enter presses the key the arrows chose (a '!' in a password must be reachable); after typing on the physical
         * keyboard it does what Enter does there: the entry is confirmed, or the app gets Enter. The on-screen OK and
         * Enter keys always confirm. */
        if (!vk_S.typed) press(vk_S.focus);
        else if (vk_S.ui.entry) confirm();
        else live_send(k);
        return true;
    default: break;
    }
    vk_S.typed = true;
    touch();
    if (vk_S.ui.entry) {
        if (k->sym == PAL_KEY_CHAR && !(k->mods & (PAL_MOD_CTRL | PAL_MOD_ALT))) entry_insert(k->cp);
        else if (k->sym == PAL_KEY_BACKSPACE) entry_backspace();
        return true;
    }
    live_send(k); /* characters, Backspace, Delete and the other keys go to the app as the physical keyboard sends them */
    return true;
}

static bool any_dirty(void) {
    if (vk_S.need_full || vk_S.dirty_text || vk_S.dirty_caret) return true;
    for (int i = 0; i < vk_S.ncells; i++)
        if (vk_S.dirty_keys[i]) return true;
    return false;
}

/* Every TICK_MS while open: flash end, caret blink (after the user is idle), the physical modifiers (shown on the caps),
 * and the poll of the app's line. */
static void tick(pal_view_t *v, uint32_t now) {
    (void)v;
    if (!vk_S.open) return;
    vk_S.now = now;
    if (vk_S.flash >= 0 && now >= vk_S.flash_until) {
        mark_key(vk_S.flash);
        vk_S.flash = -1;
    }
    if (!vk_S.fixed && now - vk_S.last_key_ms >= IDLE_MS && now - vk_S.caret_at >= CARET_MS) {
        vk_S.caret_at = now;
        vk_S.caret_on = !vk_S.caret_on;
        vk_S.dirty_caret = true;
    }
    uint8_t ph = pal_input_mods_held();
    if (ph != vk_S.phys) {
        vk_S.phys = ph;
        mark_all_keys();
    }
    if (!vk_S.fixed && !vk_S.ui.entry && (now < vk_S.fast_until || now - vk_S.last_poll >= POLL_MS)) {
        vk_S.last_poll = now;
        sync_box();
    }
    if (any_dirty()) wake();
}

/* Used by the drawing and by tests: a press of the keyboard cell idx. */
void vk_press(int idx) { press(idx); }
