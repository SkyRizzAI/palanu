/* The lock screen: opaque, the big clock and LOCKED, both centred. Nothing else is drawn, so the
 * screen matches the reference. Without a PIN, Enter twice unlocks. With security.pin
 * set, the digits typed so far show as dots under LOCKED, and "Wrong PIN" after a failed try; the keys are the same as before.
 * The unlock action is ui_wire.c's pal_ui_unlock. The lock animation and the notification summary are gone (the reference has neither).
 *
 * Layout (the reference LockScreen): a column with 8 px padding, gap 12, the clock in Bold10 (15 px cell) and LOCKED in Reg8 (11 px),
 * the column centred in both directions (Justify::Center, Align::Center). */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/desktop.h"
#include "pal/dpm.h"
#include "pal/kui.h"
#include "pal/overlay.h"
#include "pal/time.h"
#include "pal/ui.h"

#define LOCK_TICK_MS 100
#define LOCK_PAD 8
#define LOCK_GAP 12
#define LOCK_TITLE_H 15 /* Bold10 cell */
#define LOCK_CAP_H 11   /* Reg8 cell */

static struct {
    pal_ui_pin_t pin;
    int enters;      /* Enter presses toward the no-PIN unlock */
    int minute;      /* the minute last painted */
} K = {.minute = -1};
static kui_canvas_t K_KC NEMA_EXT_BSS; /* the logical canvas, in external RAM where the board has it */

void pal_ui_unlock(void); /* ui_wire.c */

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&K_KC, c->w, c->h, kui_scale_q()); /* all paper */
    kui_canvas_t *k = &K_KC;

    pal_tm_t tm;
    pal_time_local(&tm);
    K.minute = tm.minute;
    char hm[12];
    pal_ui_fmt_clock(tm.hour, tm.minute, hm, sizeof(hm));

    int inner_w = k->w - 2 * LOCK_PAD, inner_h = k->h - 2 * LOCK_PAD;
    int total = LOCK_TITLE_H + LOCK_GAP + LOCK_CAP_H;
    int y = LOCK_PAD + (inner_h - total) / 2;
    int cw = kui_text_w(&FONT_BOLD10, hm);
    kui_text(k, &FONT_BOLD10, LOCK_PAD + (inner_w - cw) / 2, y, hm, true);

    const char *label = "LOCKED";
    int lw = kui_text_w(&FONT_REG8, label);
    int ly = y + LOCK_TITLE_H + LOCK_GAP;
    kui_text(k, &FONT_REG8, LOCK_PAD + (inner_w - lw) / 2, ly, label, true);

    if (pal_ui_pin_is_set() && K.pin.len > 0) {
        /* typed digits as filled dots (kui_fill_round radius 1 gives round-looking squares at 2 px scale) */
        int n = K.pin.len, step = 8, dw = n * step - 3, dx = k->w / 2 - dw / 2, dy = ly + LOCK_CAP_H + 6;
        for (int i = 0; i < n && i < 16; i++) kui_fill(k, dx + i * step, dy, 4, 4, true);
    }
    if (pal_ui_pin_is_set() && K.pin.last_wrong) {
        const char *msg = "Wrong PIN";
        kui_text(k, &FONT_REG8, (k->w - kui_text_w(&FONT_REG8, msg)) / 2, ly + LOCK_CAP_H + 16, msg, true);
    }
    kui_present(c, k, a);
}

static void enter(pal_view_t *v) {
    (void)v;
    pal_ui_pin_reset(&K.pin);
    K.enters = 0;
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (pal_ui_pin_is_set()) {
        pal_ui_pin_event_t ev = pal_ui_pin_key(&K.pin, k, pal_ui_pin_check);
        if (ev == PAL_UI_PIN_OK) pal_ui_unlock();
        return ev != PAL_UI_PIN_NONE || k->sym == PAL_KEY_ENTER;
    }
    if (k->sym == PAL_KEY_ENTER) {
        if (++K.enters >= 2) pal_ui_unlock();
    } else {
        K.enters = 0;
    }
    return true;
}

static void exposed(pal_view_t *v) {
    (void)v;
    pal_desktop_invalidate_all();
}

/* The clock minute changes: repaint. */
static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    pal_tm_t tm;
    pal_time_local(&tm);
    if (tm.minute != K.minute) pal_desktop_invalidate();
}

/* uishow lock FRAME: the frame argument belonged to the animation and is ignored now. */
void pal_ui_lock_freeze(int frame) {
    (void)frame;
    pal_desktop_invalidate_all();
}

pal_view_t pal_lock_view = {
    .id = "lock", .title = "Locked", .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_HIDDEN, .enter = enter,
    .draw = draw, .key = key, .tick = tick, .tick_ms = LOCK_TICK_MS, .exposed = exposed,
};
