/* Athena: a desktop environment whose home screen is drawn with LVGL (docs/DESKTOPS.md).
 *
 * LVGL renders into its own RGB565 frame (direct mode: the frame persists, only dirty areas are redrawn). The desktop's canvas is 8-bit
 * indexed, so draw() copies the rendered area into it, dithering and snapping each colour to the xterm 6x6x6 cube that every theme palette carries in
 * slots 16..231. The view is an ordinary home view: the status bar, overlays, lock and power are the compositor's. The home is a clock
 * and a grid of the launcher's apps; arrows move, Enter opens. */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "nema/types.h"
#include "pal/athena.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/input.h"
#include "pal/nav.h"
#include "pal/overlay.h"
#include "pal/registry.h"
#include "pal/time.h"
#include "pal/ui.h"

#define MAX_W 320
#define MAX_H 240
#define MAX_TILES 24

/* The look: a soft indigo-to-plum wallpaper, rounded gradient app icons with a glyph each, light type, and a translucent pill at the
 * bottom. The canvas has 216 palette colours for LVGL's 65536, so draw() dithers (ordered, 4x4) while it snaps; that is what makes
 * the gradients read as smooth on the small panel. */
#define C_WALL_TOP lv_color_hex(0x1A1750)
#define C_WALL_BOT lv_color_hex(0x7A3C8C)
#define C_WHITE lv_color_hex(0xFFFFFF)

typedef struct {
    const char *id, *glyph;
    uint32_t top, bot; /* the icon's gradient */
} icon_style_t;

static const icon_style_t ICONS[] = {
    {"terminal", LV_SYMBOL_KEYBOARD, 0x5A5A63, 0x1C1C22}, {"settings", LV_SYMBOL_SETTINGS, 0xB5B5BD, 0x6E6E78},
    {"control", LV_SYMBOL_BARS, 0x4DA3FF, 0x1F5FD6},       {"notifications", LV_SYMBOL_BELL, 0xFF7A6B, 0xE0334A},
    {"monitor", LV_SYMBOL_EYE_OPEN, 0x5BE08A, 0x1E9E55},   {"files", LV_SYMBOL_DIRECTORY, 0x5CC8FF, 0x1E84E0},
    {"logs", LV_SYMBOL_EDIT, 0xFFC857, 0xF08A24},          {"applist", LV_SYMBOL_HOME, 0xB07CFF, 0x6A3FD8},
};
static const icon_style_t FALLBACK_ICON = {"", NULL, 0x8E9AAF, 0x4E5A72};

static const icon_style_t *icon_for(const char *id) {
    for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++)
        if (!strcmp(ICONS[i].id, id)) return &ICONS[i];
    return &FALLBACK_ICON;
}

static struct {
    lv_display_t *disp;
    lv_obj_t *clock, *date, *hint, *grid;
    lv_obj_t *tile[MAX_TILES];
    pal_reg_app_t app[MAX_TILES];
    int n, sel, cols, w, h;
    int last_minute;
    uint32_t last_ms;
    bool pending;
    int dx0, dy0, dx1, dy1; /* dirty union of what LVGL has drawn since the last copy */
} A;

static uint16_t s_frame[MAX_W * MAX_H] NEMA_EXT_BSS __attribute__((aligned(4)));

/* The two cube levels around v, and which one a dither threshold t (0..15) picks. */
static uint8_t snap_dither(uint8_t v, int t) {
    static const uint8_t lv[6] = {0, 95, 135, 175, 215, 255};
    int hi = 1;
    while (hi < 5 && v > lv[hi]) hi++;
    int lo = hi - 1;
    return (uint8_t)((v - lv[lo]) * 16 > t * (lv[hi] - lv[lo]) ? hi : lo);
}

static uint8_t to_index(uint16_t p, int x, int y) {
    static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    int t = bayer[(y & 3) * 4 + (x & 3)];
    uint8_t r = (uint8_t)((((p >> 11) & 31) * 255 + 15) / 31);
    uint8_t g = (uint8_t)((((p >> 5) & 63) * 255 + 31) / 63);
    uint8_t b = (uint8_t)(((p & 31) * 255 + 15) / 31);
    return (uint8_t)(16 + 36 * snap_dither(r, t) + 6 * snap_dither(g, t) + snap_dither(b, t));
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    (void)px;
    if (!A.pending) A.dx0 = a->x1, A.dy0 = a->y1, A.dx1 = a->x2, A.dy1 = a->y2;
    else {
        if (a->x1 < A.dx0) A.dx0 = a->x1;
        if (a->y1 < A.dy0) A.dy0 = a->y1;
        if (a->x2 > A.dx1) A.dx1 = a->x2;
        if (a->y2 > A.dy1) A.dy1 = a->y2;
    }
    A.pending = true;
    lv_display_flush_ready(d);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static void style_tile(int i) {
    bool on = i == A.sel;
    lv_obj_t *icon = lv_obj_get_child(A.tile[i], 0);
    lv_obj_set_style_outline_width(icon, on ? 2 : 0, 0);
    lv_obj_set_style_outline_color(icon, C_WHITE, 0);
    lv_obj_set_style_outline_pad(icon, 2, 0);
    lv_obj_set_style_outline_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(icon, on ? 272 : 256, 0); /* 256 is 100% */
    lv_obj_set_style_text_opa(lv_obj_get_child(A.tile[i], 1), on ? LV_OPA_COVER : LV_OPA_70, 0);
}

static void select_tile(int i) {
    if (A.n == 0) return;
    int old = A.sel;
    A.sel = i < 0 ? 0 : i >= A.n ? A.n - 1 : i;
    style_tile(old);
    style_tile(A.sel);
    lv_obj_scroll_to_view(A.tile[A.sel], LV_ANIM_OFF);
}

static void update_clock(void) {
    pal_tm_t t;
    pal_time_local(&t);
    A.last_minute = t.hour * 60 + t.minute;
    static const char *const DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    lv_label_set_text_fmt(A.clock, "%02d:%02d", t.hour, t.minute);
    lv_label_set_text_fmt(A.date, "%s, %d %s", DAY[t.wday % 7], t.day, MON[(t.month + 11) % 12]);
}

/* (Re)build the screen for the area and the launcher's current apps. */
static void build(int w, int h) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, C_WALL_TOP, 0);
    lv_obj_set_style_bg_grad_color(scr, C_WALL_BOT, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    A.date = lv_label_create(scr);
    lv_obj_set_style_text_font(A.date, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(A.date, C_WHITE, 0);
    lv_obj_set_style_text_opa(A.date, LV_OPA_70, 0);
    lv_obj_align(A.date, LV_ALIGN_TOP_MID, 0, 0);
    A.clock = lv_label_create(scr);
    lv_obj_set_style_text_font(A.clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(A.clock, C_WHITE, 0);
    lv_obj_align(A.clock, LV_ALIGN_TOP_MID, 0, 12);

    const int top = 64, bottom = 26, ICON = 40, CELL_H = 58, CELL_W = 64;
    A.grid = lv_obj_create(scr);
    lv_obj_remove_style_all(A.grid);
    lv_obj_set_pos(A.grid, 0, top);
    lv_obj_set_size(A.grid, w, h - top - bottom);
    lv_obj_add_flag(A.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(A.grid, LV_SCROLLBAR_MODE_OFF);

    A.cols = w / CELL_W;
    if (A.cols < 1) A.cols = 1;
    int margin = (w - A.cols * CELL_W) / 2;
    A.n = 0;
    for (size_t i = 0; i < psys_launcher_count() && A.n < MAX_TILES; i++) {
        if (!psys_launcher_get(i, &A.app[A.n])) continue;
        const icon_style_t *st = icon_for(A.app[A.n].id);
        lv_obj_t *t = lv_obj_create(A.grid); /* a cell: child 0 is the icon, child 1 the name */
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, CELL_W, CELL_H);
        lv_obj_set_pos(t, margin + (A.n % A.cols) * CELL_W, 4 + (A.n / A.cols) * CELL_H);
        A.tile[A.n] = t;
        lv_obj_t *icon = lv_obj_create(t);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, ICON, ICON);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_set_style_radius(icon, 10, 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(st->top), 0);
        lv_obj_set_style_bg_grad_color(icon, lv_color_hex(st->bot), 0);
        lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_shadow_width(icon, 8, 0);
        lv_obj_set_style_shadow_offset_y(icon, 2, 0);
        lv_obj_set_style_shadow_opa(icon, LV_OPA_40, 0);
        lv_obj_set_style_shadow_color(icon, lv_color_hex(0x000000), 0);
        lv_obj_t *g = lv_label_create(icon);
        lv_obj_set_style_text_font(g, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(g, C_WHITE, 0);
        if (st->glyph) lv_label_set_text(g, st->glyph);
        else {
            char init[2] = {A.app[A.n].name[0] ? A.app[A.n].name[0] : '?', 0};
            lv_label_set_text(g, init);
        }
        lv_obj_center(g);
        lv_obj_t *name = lv_label_create(t);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(name, C_WHITE, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, CELL_W - 4, 14); /* one line: a longer name ends in dots */
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(name, A.app[A.n].name);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, 0);
        A.n++;
    }
    if (A.sel >= A.n) A.sel = 0;
    for (int i = 0; i < A.n; i++) style_tile(i);

    lv_obj_t *pill = lv_obj_create(scr); /* a translucent bar, like a dock: the keys, in plain words */
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, w - 48, 20);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, C_WHITE, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_20, 0);
    A.hint = lv_label_create(pill);
    lv_obj_set_style_text_font(A.hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.hint, C_WHITE, 0);
    lv_label_set_text(A.hint, "Athena   |   arrows move   |   Enter opens");
    lv_obj_center(A.hint);
    update_clock();
}

static void ensure(int w, int h) {
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;
    if (A.disp && A.w == w && A.h == h) return;
    if (!A.disp) {
        lv_init();
        A.disp = lv_display_create(w, h);
        lv_display_set_color_format(A.disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_flush_cb(A.disp, flush);
    } else lv_display_set_resolution(A.disp, w, h);
    lv_display_set_buffers(A.disp, s_frame, NULL, (uint32_t)w * (uint32_t)h * 2u, LV_DISPLAY_RENDER_MODE_DIRECT);
    A.w = w, A.h = h;
    build(w, h);
}

static void enter(pal_view_t *v) {
    (void)v;
    if (A.disp) build(A.w, A.h); /* the launcher's apps may have changed */
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t area, bool full) {
    (void)v;
    ensure(area.w, area.h);
    lv_timer_handler();
    int x0 = 0, y0 = 0, x1 = A.w - 1, y1 = A.h - 1;
    if (!full && A.pending) x0 = A.dx0, y0 = A.dy0, x1 = A.dx1, y1 = A.dy1;
    else if (!full) return;
    A.pending = false;
    for (int y = y0; y <= y1 && y < area.h; y++) {
        int cy = area.y + y;
        if (cy < 0 || cy >= c->h) continue;
        for (int x = x0; x <= x1 && x < area.w; x++) {
            int cx = area.x + x;
            if (cx < 0 || cx >= c->w) continue;
            c->px[cy * c->stride + cx] = to_index(s_frame[y * A.w + x], x, y);
        }
    }
    pal_mark(c, area.x + x0, area.y + y0, x1 - x0 + 1, y1 - y0 + 1);
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v;
    if (!A.disp) return;
    lv_tick_inc(A.last_ms ? now - A.last_ms : 0);
    A.last_ms = now;
    pal_tm_t t;
    pal_time_local(&t);
    if (t.hour * 60 + t.minute != A.last_minute) update_clock();
    lv_timer_handler();
    if (A.pending) pal_desktop_invalidate();
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    switch (pal_nav_of(k)) {
    case PAL_NAV_LEFT: select_tile(A.sel - 1); break;
    case PAL_NAV_RIGHT: select_tile(A.sel + 1); break;
    case PAL_NAV_UP: select_tile(A.sel - A.cols >= 0 ? A.sel - A.cols : A.sel); break;
    case PAL_NAV_DOWN: select_tile(A.sel + A.cols < A.n ? A.sel + A.cols : A.sel); break;
    case PAL_NAV_OK:
        if (A.n > 0) pal_launcher_run(&A.app[A.sel]);
        return true;
    default: return false;
    }
    lv_timer_handler();
    pal_desktop_invalidate();
    return true;
}

static pal_view_t ATHENA_VIEW = {.id = "athena", .title = "Athena", .flags = PAL_VIEW_HIDDEN, .enter = enter, .draw = draw, .key = key,
                                 .tick = tick, .tick_ms = 33};

static bool s_view_registered;

static bool athena_start(void) {
    if (!s_view_registered) {
        nema_err_t r = pal_desktop_register(&ATHENA_VIEW);
        if (r != NEMA_OK && r != NEMA_ERR_EXIST) return false;
        s_view_registered = true;
    }
    pal_desktop_set_home("athena");
    /* the grid uses the arrows and Enter itself: the classic home keys go away (Settings, Files and the rest are tiles) */
    pal_desktop_bind_home_key(PAL_KEY_ENTER, NULL);
    pal_desktop_bind_home_key(PAL_KEY_UP, NULL);
    pal_desktop_bind_home_key(PAL_KEY_DOWN, NULL);
    return true;
}

static const pal_desktop_env_t ATHENA = {.id = "athena", .title = "Athena", .about = "LVGL home: clock and app grid (simulator build)",
                                         .start = athena_start};

void pal_athena_register(void) { (void)pal_desktops_register(&ATHENA); }
