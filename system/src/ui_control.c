/* Mission Control (the reference MissionControlScreen, the reference sources): opaque on the
 * desktop, under the status bar. Five octagonal tiles (Dark, Wi-Fi, Lock, Settings, Restart) in a 3 x 2 grid and two vertical
 * pill sliders on the right: brightness (live) and volume. The focused tile is filled. Logical canvas, as the other kui files;
 * the layout is the landscape one (see ui_control_model.c). The icons are the reference's own bitmaps, except the lock and the gear,
 * which are redrawn here (the earlier lock design is not carried over). Volume has no audio output on any board, so its value
 * is kept here and changes nothing. Restart asks first (a dialog), as before. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/display.h"
#include "pal/kui.h"
#include "pal/radio.h"
#include "pal/settings.h"
#include "pal/svc.h"
#include "pal/theme.h"
#include "pal/ui.h"
#include "settings_api.h"
#include "ui_control.h"

#define TILE 30    /* the reference: the square side, from the screen width (ts) */
#define GAP 6
#define PAD 10
#define BAR_W 12   /* ts * 2 / 5 */
#define TILE_R 6   /* The reference cornerRadius 6 */
#define BAR_R 4    /* a third of the bar width */
static int s_ts = TILE; /* the tile side of the last layout: from the canvas width, so it shrinks at the larger UI scales */
#define GRID_H (2 * s_ts + GAP)

/* 8 rows, one string each; '#' is a lit pixel. */
typedef struct {
    const char *const *rows;
    int w;
} glyph_t;

static const char *const G_DARK[8] = {"..####..", ".###..#.", "####...#", "####...#", "####...#", "####...#", ".###..#.", "..####.."};
static const char *const G_RESTART[8] = {"..####..", ".#....#.", "#......#", "#....###", "#.....#.", "#.......", ".#....#.", "..####.."};
static const char *const G_SUN[8] = {"...##...", ".#.##.#.", "..####..", "########", "########", "..####..", ".#.##.#.", "...##..."};
static const char *const G_SPK[8] = {"...#....", "..##..#.", "####.#.#", "####.#.#", "####.#.#", "####.#.#", "..##..#.", "...#...."};
static const char *const G_WIFI[8] = {"..........", "..######..", ".#......#.", "...####...", "..#....#..", "....##....", "..........", "....#....."};
static const char *const G_LOCK[8] = {"..#####..", ".#.....#.", ".#.....#.", "#########", "#.......#", "#...#...#", "#...#...#", "#########"};
static const char *const G_GEAR[8] = {"...##...", ".#.##.#.", "#..##..#", "##....##", "##....##", "#..##..#", ".#.##.#.", "...##..."};

static struct {
    ctl_item_t focus;
    int volume; /* 0..100, kept only (no audio output) */
} C = {.volume = 100};
static kui_canvas_t C_KC NEMA_EXT_BSS; /* the logical canvas, in external RAM where the board has it */

/* Draws a glyph with its top-left at (x, y). Over an ink pixel the glyph shows paper, and the other pixels take ink, when
 * invert is set (the slider glyphs); otherwise the glyph is drawn in colour on. */
static void glyph(kui_canvas_t *k, int x, int y, const glyph_t *g, bool on, bool invert) {
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < g->w; c++) {
            if (g->rows[r][c] != '#') continue;
            if (invert) kui_pixel(k, x + c, y + r, !kui_get(k, x + c, y + r));
            else kui_pixel(k, x + c, y + r, on);
        }
}

static void tile(kui_canvas_t *k, int x, int y, ctl_item_t it, const glyph_t *g) {
    bool f = C.focus == it;
    if (f) kui_fill_round(k, x + 1, y + 1, s_ts - 2, s_ts - 2, 4, true); /* The reference fills inside the 1 px border */
    else kui_round(k, x, y, s_ts, s_ts, TILE_R, true);
    glyph(k, x + (s_ts - g->w) / 2, y + (s_ts - 8) / 2, g, !f, false);
}

/* A vertical pill: an outline, filled from the bottom to the value, with its glyph in the middle (inverted). */
static void slider(kui_canvas_t *k, int x, int y, int value, const glyph_t *g) {
    kui_round(k, x, y, BAR_W, GRID_H, BAR_R, true);
    int fh = value * GRID_H / 100;
    if (fh >= GRID_H) kui_fill_round(k, x, y, BAR_W, GRID_H, BAR_R, true);
    else if (fh > 0) kui_fill_round(k, x, y + GRID_H - fh, BAR_W, fh, BAR_R, true);
    glyph(k, x + (BAR_W - g->w) / 2, y + (GRID_H - 8) / 2, g, true, true);
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&C_KC, c->w, c->h, kui_scale_q()); /* paper */
    kui_canvas_t *k = &C_KC;
    int cy = kui_lg(k, a.y), ch = kui_lg(k, a.y + a.h) - cy; /* the content area: under the status bar */

    /* The reference root: a row with padding 10, centred. Tile side from the width and the height (ts), as in the screen code. */
    int ts = (5 * (k->w - 2 * PAD - 4 * GAP)) / 19;
    if (ts > TILE) ts = TILE;
    s_ts = ts;
    int grid_w = 3 * ts + 2 * GAP, total = grid_w + GAP + BAR_W + GAP + BAR_W;
    int x0 = PAD + (k->w - 2 * PAD - total) / 2;
    int y0 = cy + PAD + (ch - 2 * PAD - GRID_H) / 2;

    tile(k, x0, y0, CTL_DARK, &(glyph_t){G_DARK, 8});
    tile(k, x0 + ts + GAP, y0, CTL_WIFI, &(glyph_t){G_WIFI, 10});
    tile(k, x0 + 2 * (ts + GAP), y0, CTL_LOCK, &(glyph_t){G_LOCK, 9});
    tile(k, x0, y0 + ts + GAP, CTL_SETTINGS, &(glyph_t){G_GEAR, 8});
    tile(k, x0 + ts + GAP, y0 + ts + GAP, CTL_RESTART, &(glyph_t){G_RESTART, 8});

    int bx = x0 + grid_w + GAP;
    slider(k, bx, y0, pal_display_brightness(), &(glyph_t){G_SUN, 8});
    slider(k, bx + BAR_W + GAP, y0, C.volume, &(glyph_t){G_SPK, 8});
    kui_present(c, k, a);
}

static void enter(pal_view_t *v) {
    (void)v;
    C.focus = CTL_DARK;
}

static void set_brightness(int delta) {
    int val = pal_display_brightness() + delta;
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    char txt[8];
    snprintf(txt, sizeof(txt), "%d", val);
    (void)psys_settings_set("display.brightness", txt);
}

static void toggle_radio(pal_radio_t r) {
    pal_radio_state_t st = pal_radio_state(r);
    const char *id = r == PAL_RADIO_WIFI ? "radio-wifi" : "radio-ble";
    if (st == PAL_RADIO_ON || st == PAL_RADIO_STARTING) (void)psys_svc_stop(id);
    else (void)psys_svc_start(id); /* a refusal (memory) is retried by the supervisor and shown as Off */
}

static void on_restart(bool ok, void *ctx) {
    (void)ctx;
    if (ok) pal_ui_splash_restart(); /* the restart splash, then the board restarts (ui_splash.c) */
}

static void fire(ctl_item_t it) {
    switch (it) {
    case CTL_WIFI: toggle_radio(PAL_RADIO_WIFI); break;
    case CTL_DARK: (void)psys_settings_set("ui.dark", pal_theme_dark() ? "0" : "1"); break;
    case CTL_LOCK:
        pal_desktop_home(); /* back to the desktop first, as the reference does, then lock */
        pal_ui_lock_now();
        break;
    case CTL_SETTINGS: pal_desktop_open("settings"); break;
    case CTL_RESTART:
        (void)pal_dialog_confirm("Restart", "Restart PalanuOS now?", "Restart", "Cancel", on_restart, NULL);
        break;
    default: break;
    }
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    bool slider = C.focus == CTL_BRIGHT || C.focus == CTL_VOL;
    switch (k->sym) {
    case PAL_KEY_ESC:
        pal_desktop_home();
        return true;
    case PAL_KEY_ENTER:
        if (!slider) fire(C.focus);
        return true;
    case PAL_KEY_UP:
    case PAL_KEY_DOWN: {
        int d = k->sym == PAL_KEY_UP ? 1 : -1; /* the reference: on a slider Prev raises and Next lowers */
        if (C.focus == CTL_BRIGHT) set_brightness(d * CTL_BRIGHT_STEP);
        else if (C.focus == CTL_VOL) C.volume = C.volume + d * CTL_VOL_STEP < 0 ? 0 : C.volume + d * CTL_VOL_STEP > 100 ? 100 : C.volume + d * CTL_VOL_STEP;
        else C.focus = pal_ui_ctl_move(C.focus, k->sym == PAL_KEY_UP ? CTL_UP : CTL_DOWN);
        return true;
    }
    case PAL_KEY_LEFT:
    case PAL_KEY_RIGHT:
        C.focus = pal_ui_ctl_move(C.focus, k->sym == PAL_KEY_LEFT ? CTL_LEFT : CTL_RIGHT);
        return true;
    default:
        return false;
    }
}

pal_view_t pal_control_view = {
    .id = "control", .title = "Control", .flags = 0, .enter = enter, .draw = draw, .key = key,
};
