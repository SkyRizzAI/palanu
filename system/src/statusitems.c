/* PalanuOS status items that the desktop owns: the unread notification dot with its count, the lock, Bluetooth, and
 * the card. Each reads its owner's state (the notify ring, the power manager, the status snapshot) and draws nothing when it is
 * off, so an idle bar looks as it did before. Owners of other items register their own (pal_status_item_register). */
#include <stdio.h>

#include "desktop_int.h"
#include "pal/dpm.h"
#include "pal/input.h"
#include "pal/notify.h"

/* A padlock 5 by 7 pixels. */
static int draw_lock(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st) {
    (void)t, (void)st;
    if (!pal_dpm_locked()) return 0;
    int y = (h - 7) / 2;
    pal_fill(c, x + 1, y, 3, 1, PAL_C_ON_ACCENT);
    pal_fill(c, x, y + 1, 1, 3, PAL_C_ON_ACCENT);
    pal_fill(c, x + 4, y + 1, 1, 3, PAL_C_ON_ACCENT);
    pal_rect(c, x, y + 3, 5, 4, PAL_C_ON_ACCENT);
    return 6;
}

/* A Bluetooth rune 5 by 7 pixels. */
static int draw_ble(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st) {
    (void)t;
    if (!st->ble) return 0;
    int y = (h - 7) / 2;
    pal_fill(c, x + 2, y, 1, 7, PAL_C_ON_ACCENT);
    pal_line(c, x + 2, y, x + 4, y + 2, PAL_C_ON_ACCENT);
    pal_line(c, x + 4, y + 2, x, y + 4, PAL_C_ON_ACCENT);
    pal_line(c, x, y + 2, x + 4, y + 4, PAL_C_ON_ACCENT);
    pal_line(c, x + 4, y + 4, x + 2, y + 6, PAL_C_ON_ACCENT);
    return 6;
}

/* A memory card 6 by 8 pixels with its notched corner. */
static int draw_sd(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st) {
    (void)t;
    if (!st->sd) return 0;
    int y = (h - 8) / 2;
    pal_rect(c, x, y, 6, 8, PAL_C_ON_ACCENT);
    pal_fill(c, x, y, 3, 1, PAL_C_ON_ACCENT);
    pal_fill(c, x + 1, y + 5, 4, 2, PAL_C_ON_ACCENT);
    return 7;
}

/* A dot and the unread count in the caption font; nothing when everything has been read. */
static int draw_notify(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st) {
    (void)st;
    int n = psys_notify_unread_count();
    if (n <= 0) return 0;
    char buf[8];
    if (n > 9) snprintf(buf, sizeof(buf), "9+");
    else snprintf(buf, sizeof(buf), "%d", n);
    const pal_font_t *f = t->tokens.caption;
    int y = (h - 5) / 2;
    pal_fill_circle(c, x + 2, y + 2, 2, PAL_C_ON_ACCENT);
    int w = pal_text(c, f, x + 5, (h - f->h) / 2, buf, PAL_C_ON_ACCENT, PAL_TRANSPARENT) - x;
    return w;
}

/* The sticky keyboard modes that are on, one small mark each: A (Caps Lock), # (symbols), 1 (digits), ^ (Ctrl for the next key). */
static int draw_input_modes(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st) {
    (void)st;
    static const struct {
        pal_input_mode_t mode;
        const char *mark;
    } MARKS[] = {{PAL_INPUT_CAPS, "A"}, {PAL_INPUT_SYM, "#"}, {PAL_INPUT_NUM, "1"}, {PAL_INPUT_CTRL, "^"}};
    const pal_font_t *f = t->tokens.caption;
    int w = 0;
    for (size_t i = 0; i < sizeof(MARKS) / sizeof(MARKS[0]); i++) {
        if (!pal_input_mode(MARKS[i].mode)) continue;
        int tw = pal_text_w(f, MARKS[i].mark);
        pal_rect(c, x + w, (h - f->h) / 2 - 1, tw + 4, f->h + 2, PAL_C_ON_ACCENT);
        pal_text(c, f, x + w + 2, (h - f->h) / 2, MARKS[i].mark, PAL_C_ON_ACCENT, PAL_TRANSPARENT);
        w += tw + 6;
    }
    return w;
}

static const pal_status_item_t BUILTIN[] = {
    {"input", draw_input_modes},
    {"notify", draw_notify},
    {"lock", draw_lock},
    {"ble", draw_ble},
    {"sd", draw_sd},
};

size_t pal_status_builtin_items(const pal_status_item_t **out) {
    *out = BUILTIN;
    return sizeof(BUILTIN) / sizeof(BUILTIN[0]);
}
