/* Keyboard: the device types into another computer or phone (ADR-0018). While it is in front it advertises as a Bluetooth keyboard
 * (pal/hid.h); once the other side has paired, every key goes there. Holding the cycle key leaves, as it leaves every app, and
 * leaving gives the radio back. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/desktop.h"
#include "pal/hid.h"
#include "pal/theme.h"
#include "views.h"

#define NAME "PalanuOS Keyboard"

static struct {
    bool on;
    pal_hid_state_t shown;
    uint32_t sent;
    char err[64];
    char last[16];
} K;

static void enter(pal_view_t *v) {
    (void)v;
    K.sent = 0, K.err[0] = K.last[0] = 0;
    const pal_hid_ops_t *h = pal_hid();
    if (!h) {
        snprintf(K.err, sizeof(K.err), "This device has no keyboard transport");
        return;
    }
    nema_err_t r = h->start(NAME);
    if (r != NEMA_OK) snprintf(K.err, sizeof(K.err), "Bluetooth did not start: %s", pal_strerror(r));
    K.on = r == NEMA_OK;
    K.shown = PAL_HID_OFF;
}

static void leave(pal_view_t *v) {
    (void)v;
    const pal_hid_ops_t *h = pal_hid();
    if (K.on && h) h->stop();
    K.on = false;
}

static pal_hid_state_t state(void) {
    const pal_hid_ops_t *h = pal_hid();
    return K.on && h ? h->state() : PAL_HID_OFF;
}

static void centre(pal_canvas_t *c, const pal_font_t *f, pal_rect_t a, int y, const char *s, uint8_t col) {
    pal_text(c, f, a.x + (a.w - pal_text_w(f, s)) / 2, y, s, col, PAL_TRANSPARENT);
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_PAPER);
    int y = a.y + a.h / 2 - 40;
    pal_hid_state_t st = state();
    const char *head = K.err[0] ? "Not available" : st == PAL_HID_CONNECTED ? "Connected" : "Waiting for a computer";
    centre(c, t->tokens.title, a, y, head, PAL_C_INK);
    y += t->tokens.title->h + 8;
    char line[80];
    if (K.err[0]) snprintf(line, sizeof(line), "%s", K.err);
    else if (st == PAL_HID_CONNECTED) {
        char peer[40] = "";
        const pal_hid_ops_t *h = pal_hid();
        if (h && h->peer) h->peer(peer, sizeof(peer));
        snprintf(line, sizeof(line), "%s%s%u keys sent", peer, peer[0] ? "  -  " : "", (unsigned)K.sent);
    } else {
        centre(c, t->tokens.body, a, y, "In its Bluetooth settings, pair", PAL_C_INK);
        y += t->tokens.body->h + 2;
        snprintf(line, sizeof(line), "\"%s\"", NAME);
    }
    centre(c, t->tokens.body, a, y, line, PAL_C_INK);
    y += t->tokens.body->h + 4;
    if (K.last[0]) {
        snprintf(line, sizeof(line), "last: %s", K.last);
        centre(c, t->tokens.caption, a, y, line, PAL_C_DIM);
    }
    centre(c, t->tokens.caption, a, a.y + a.h - t->tokens.caption->h - 4, "Every key goes there. Hold the cycle key to leave.",
           PAL_C_DIM);
}

static void describe_key(const pal_key_t *k, char *out, size_t cap) {
    static const char *const NAMES[] = {"", "", "Enter", "Esc", "Backspace", "Tab", "Delete", "Up", "Down", "Left", "Right",
                                        "Home", "End", "PgUp", "PgDn"};
    const char *ctrl = k->mods & PAL_MOD_CTRL ? "Ctrl+" : "", *alt = k->mods & PAL_MOD_ALT ? "Alt+" : "";
    if (k->sym == PAL_KEY_CHAR) snprintf(out, cap, "%s%s%c", ctrl, alt, k->cp == ' ' ? '_' : (char)k->cp);
    else if (k->sym < sizeof(NAMES) / sizeof(NAMES[0])) snprintf(out, cap, "%s%s%s", ctrl, alt, NAMES[k->sym]);
    else out[0] = 0;
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (K.err[0]) return k->sym != PAL_KEY_ESC ? true : (pal_app_exit(), true);
    nema_err_t r = pal_hid_type(k);
    if (r == NEMA_OK) {
        K.sent++;
        describe_key(k, K.last, sizeof(K.last));
        pal_desktop_invalidate();
    }
    return true; /* every key belongs to the other side, sent or not */
}

static void pump(pal_view_t *v) {
    (void)v;
    pal_hid_state_t st = state();
    if (st != K.shown) {
        K.shown = st;
        pal_desktop_invalidate();
    }
}

pal_view_t pal_hidkbd_view = {.id = "keyboard", .title = "Keyboard", .enter = enter, .leave = leave, .draw = draw, .key = key,
                              .pump = pump};
