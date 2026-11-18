/* Athena: a desktop environment drawn with LVGL in full colour (docs/DESKTOPS.md).
 *
 * Athena owns its chrome (pal_desktop_chrome_t): its own status bar on top of every screen, its own global keys (Fn twice opens its
 * Keyboard sheet, a tap of the cycle key its Recent apps sheet, holding it goes home), and its own home. LVGL renders one RGB565 frame
 * the size of the panel (direct mode: the frame persists, only dirty areas are redrawn) and Athena sends it to the panel itself
 * (pal_display_present_rgb565): no palette, no dithering.
 *
 * Two cases. While the home or a sheet is in front, Athena owns the whole screen and the compositor draws nothing. While an app (or a
 * dialog, the lock screen, a toast) is in front, the compositor draws it below Athena's bar (top_px rows are kept for the bar) and
 * Athena sends only its bar. When something covers the home (a toast, the on-screen keyboard), the home is copied into the 8-bit canvas
 * by the view's draw, snapped to the palette's colour cube. */
#include <stdio.h>
#include <string.h>

#include "../desktop_int.h"
#include "lvgl.h"
#include "nema/types.h"
#include "pal/athena.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/display.h"
#include "pal/dpm.h"
#include "pal/input.h"
#include "pal/menu.h"
#include "pal/nav.h"
#include "pal/overlay.h"
#include "pal/registry.h"
#include "pal/time.h"
#include "pal/toast.h"
#include "pal/ui.h"
#include "pal/vk.h"

#define MAX_W 320
#define MAX_H 240
#define MAX_TILES 24
#define MAX_ROWS 8
#define BAR_H 20
#define MAX_PAGE_ROWS 40
#define VALUE_MAX 40

#define C_WALL_TOP lv_color_hex(0x1A1750)
#define C_WALL_BOT lv_color_hex(0x7A3C8C)
#define C_WHITE lv_color_hex(0xFFFFFF)
#define C_BLACK lv_color_hex(0x000000)
#define C_SHEET lv_color_hex(0xF2F2F7) /* a grouped-list background */
#define C_ROW lv_color_hex(0xFFFFFF)
#define C_BLUE lv_color_hex(0x0A84FF)
#define C_INK lv_color_hex(0x1C1C1E)
#define C_GREY lv_color_hex(0x8E8E93)
#define C_GREEN lv_color_hex(0x34C759)
#define C_RED lv_color_hex(0xFF3B30)
#define C_TRACK lv_color_hex(0xE5E5EA)

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

typedef enum { SHEET_NONE, SHEET_KEYBOARD, SHEET_SHORTCUTS, SHEET_RECENTS } sheet_t;

static struct {
    lv_display_t *disp;
    int w, h;
    /* status bar (on LVGL's top layer, above everything) */
    lv_obj_t *bar, *bar_time, *bar_right;
    bool bar_solid;
    pal_status_t st;
    /* home */
    lv_obj_t *home, *clock, *date, *grid;
    lv_obj_t *tile[MAX_TILES];
    pal_reg_app_t app[MAX_TILES];
    int n, sel, cols, last_minute;
    /* sheet */
    lv_obj_t *sheet, *sheet_title, *sheet_hint;
    lv_obj_t *row[MAX_ROWS];
    char row_id[MAX_ROWS][24]; /* recents: the view each row opens */
    int nrows, row_sel;
    bool row_info; /* shortcuts: the rows are not choices */
    sheet_t kind;
    /* toolkit pages: a bound menu (pal_menu_bind) drawn as a grouped list */
    lv_obj_t *page, *page_title, *page_list, *page_empty;
    lv_obj_t *prow[MAX_PAGE_ROWS], *pval[MAX_PAGE_ROWS];
    int pidx[MAX_PAGE_ROWS]; /* the page row each drawn row shows */
    char ptext[MAX_PAGE_ROWS][VALUE_MAX];
    int pn, psel;
    pal_menu_t *pm;
    pal_menu_page_t *ppage;
    size_t prows;
    uint32_t pvals_ms;
    int mode; /* 0 home, 1 toolkit page, 2 an app drawn by the compositor */
    /* sending */
    uint32_t last_ms;
    bool pending, force, owned, display_on;
    int dx0, dy0, dx1, dy1; /* dirty union of what LVGL has drawn since the last send */
} A NEMA_EXT_BSS;

static uint16_t s_frame[MAX_W * MAX_H] NEMA_EXT_BSS __attribute__((aligned(4)));

/* ---- frame ---- */

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

/* For the 8-bit canvas (the home under a toast or the on-screen keyboard): ordered dithering onto the palette's 6x6x6 cube. */
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

/* ---- status bar ---- */

static const char *battery_glyph(int pct) {
    if (pct >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60) return LV_SYMBOL_BATTERY_3;
    if (pct >= 35) return LV_SYMBOL_BATTERY_2;
    if (pct >= 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* The status costs an ADC sample for the battery: read it once a second at most, as the classic bar does (30 reads a second
 * tripped the interrupt watchdog on hhv2). */
static void bar_update(bool always, uint32_t now) {
    static uint32_t last_read;
    if (!always && now - last_read < 1000u) return;
    last_read = now;
    pal_status_t st = pal_desktop_status();
    if (!always && st.hour == A.st.hour && st.minute == A.st.minute && st.battery == A.st.battery && st.wifi == A.st.wifi &&
        st.ble == A.st.ble && st.link == A.st.link)
        return;
    A.st = st;
    lv_label_set_text_fmt(A.bar_time, "%02d:%02d", st.hour, st.minute);
    char right[48];
    int n = 0;
    right[0] = 0;
    if (st.link) n += snprintf(right + n, sizeof(right) - (size_t)n, "%s  ", LV_SYMBOL_LOOP);
    if (st.ble) n += snprintf(right + n, sizeof(right) - (size_t)n, "%s  ", LV_SYMBOL_BLUETOOTH);
    if (st.wifi > 0) n += snprintf(right + n, sizeof(right) - (size_t)n, "%s  ", LV_SYMBOL_WIFI);
    if (st.battery >= 0) snprintf(right + n, sizeof(right) - (size_t)n, "%d%%  %s", st.battery, battery_glyph(st.battery));
    lv_label_set_text(A.bar_right, right);
}

/* Over the home the bar is clear (the wallpaper shows through); over an app it is solid black, as the app's own top edge. */
static void bar_style(bool solid) {
    if (solid == A.bar_solid) return;
    A.bar_solid = solid;
    lv_obj_set_style_bg_opa(A.bar, solid ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void build_bar(void) {
    A.bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(A.bar);
    lv_obj_set_size(A.bar, A.w, BAR_H);
    lv_obj_set_pos(A.bar, 0, 0);
    lv_obj_set_style_bg_color(A.bar, C_BLACK, 0);
    lv_obj_set_style_bg_opa(A.bar, LV_OPA_TRANSP, 0);
    A.bar_solid = false;
    A.bar_time = lv_label_create(A.bar);
    lv_obj_set_style_text_font(A.bar_time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_time, C_WHITE, 0);
    lv_obj_align(A.bar_time, LV_ALIGN_LEFT_MID, 12, 0);
    A.bar_right = lv_label_create(A.bar);
    lv_obj_set_style_text_font(A.bar_right, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_right, C_WHITE, 0);
    lv_obj_align(A.bar_right, LV_ALIGN_RIGHT_MID, -10, 0);
    bar_update(true, 0);
}

/* ---- home ---- */

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
    static const char *const MON[] = {"January", "February", "March",     "April",   "May",      "June",
                                      "July",    "August",   "September", "October", "November", "December"};
    lv_label_set_text_fmt(A.clock, "%02d:%02d", t.hour, t.minute);
    lv_label_set_text_fmt(A.date, "%s, %d %s", DAY[t.wday % 7], t.day, MON[(t.month + 11) % 12]);
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

/* The home below the bar: the date and a large clock, a grid of the launcher's apps, and a translucent bar with the keys. */
static void build_home(void) {
    int w = A.w, h = A.h - BAR_H;
    if (A.home) lv_obj_delete(A.home);
    A.home = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(A.home);
    lv_obj_set_pos(A.home, 0, BAR_H);
    lv_obj_set_size(A.home, w, h);
    lv_obj_remove_flag(A.home, LV_OBJ_FLAG_SCROLLABLE);

    A.date = text(A.home, &lv_font_montserrat_16, C_WHITE);
    lv_obj_set_style_text_opa(A.date, LV_OPA_70, 0);
    lv_obj_align(A.date, LV_ALIGN_TOP_MID, 0, 0);
    A.clock = text(A.home, &lv_font_montserrat_48, C_WHITE);
    lv_obj_align(A.clock, LV_ALIGN_TOP_MID, 0, 12);

    const int top = 64, bottom = 26, ICON = 40, CELL_H = 58, CELL_W = 64;
    A.grid = lv_obj_create(A.home);
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
        lv_obj_set_style_shadow_color(icon, C_BLACK, 0);
        lv_obj_t *g = text(icon, &lv_font_montserrat_20, C_WHITE);
        if (st->glyph) lv_label_set_text(g, st->glyph);
        else {
            char init[2] = {A.app[A.n].name[0] ? A.app[A.n].name[0] : '?', 0};
            lv_label_set_text(g, init);
        }
        lv_obj_center(g);
        lv_obj_t *name = text(t, &lv_font_montserrat_12, C_WHITE);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, CELL_W - 4, 14); /* one line: a longer name ends in dots */
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(name, A.app[A.n].name);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, 0);
        A.n++;
    }
    if (A.sel >= A.n) A.sel = 0;
    for (int i = 0; i < A.n; i++) style_tile(i);

    lv_obj_t *pill = lv_obj_create(A.home); /* a translucent bar, like a dock: the keys, in plain words */
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, w - 32, 20);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, C_WHITE, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_20, 0);
    lv_obj_t *hint = text(pill, &lv_font_montserrat_12, C_WHITE);
    lv_label_set_text(hint, "Enter open  |  Fn Fn keyboard  |  Cycle recents");
    lv_obj_center(hint);
    update_clock();
}

/* ---- sheets: Keyboard (Fn twice), Shortcuts, Recent apps (cycle tap) ---- */

static void style_row(int i) {
    bool on = !A.row_info && i == A.row_sel;
    lv_obj_set_style_bg_color(A.row[i], on ? C_BLUE : C_ROW, 0);
    lv_obj_t *l = lv_obj_get_child(A.row[i], 0);
    lv_obj_set_style_text_color(l, on ? C_WHITE : C_INK, 0);
}

static void sheet_close(void) {
    if (A.kind == SHEET_NONE) return;
    A.kind = SHEET_NONE;
    lv_obj_add_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    A.force = true;
}

static void sheet_open(sheet_t kind, const char *title, const char *const *rows, int n, bool info) {
    lv_obj_clean(A.sheet);
    A.kind = kind;
    A.row_info = info;
    A.nrows = n > MAX_ROWS ? MAX_ROWS : n;
    A.row_sel = 0;
    A.sheet_title = text(A.sheet, &lv_font_montserrat_20, C_INK);
    lv_label_set_text(A.sheet_title, title);
    lv_obj_set_pos(A.sheet_title, 14, 10);
    int rh = info ? 20 : 26, y = 40;
    for (int i = 0; i < A.nrows; i++) {
        lv_obj_t *r = lv_obj_create(A.sheet);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, A.w - 16 - 20, rh);
        lv_obj_set_pos(r, 10, y + i * (rh + 4));
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_t *l = text(r, info ? &lv_font_montserrat_12 : &lv_font_montserrat_16, C_INK);
        lv_label_set_text(l, rows[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        A.row[i] = r;
        style_row(i);
    }
    A.sheet_hint = text(A.sheet, &lv_font_montserrat_12, C_GREY);
    lv_label_set_text(A.sheet_hint, info ? "Esc back" : "arrows choose  |  Enter open  |  Esc close");
    lv_obj_align(A.sheet_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_remove_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    A.force = true;
}

static void open_keyboard_sheet(void) {
    static const char *const ROWS[] = {LV_SYMBOL_KEYBOARD "   On-screen keyboard", LV_SYMBOL_LIST "   Keyboard shortcuts",
                                       LV_SYMBOL_SETTINGS "   Settings", LV_SYMBOL_CLOSE "   Close"};
    sheet_open(SHEET_KEYBOARD, "Keyboard", ROWS, 4, false);
}

static void open_shortcuts_sheet(void) {
    static const char *const ROWS[] = {
        "Fn + W A S D        move (arrows)",  "Enter / Esc        open / back",       "Fn twice        Keyboard (this menu)",
        "Cycle tap        Recent apps",       "Cycle hold        Home",              "Ctrl + C        stop (in the Terminal)",
    };
    sheet_open(SHEET_SHORTCUTS, "Keyboard shortcuts", ROWS, 6, true);
}

static void open_recents_sheet(void) {
    static char labels[MAX_ROWS][40];
    const char *rows[MAX_ROWS];
    int n = 0;
    for (int i = 0; i < desk_mru_count() && n < MAX_ROWS - 1; i++) {
        pal_view_t *v = desk_mru_at(i);
        if (!v || (v->flags & PAL_VIEW_HIDDEN) || !strcmp(v->id, "athena")) continue;
        nema_strlcpy(A.row_id[n], v->id, sizeof(A.row_id[n]));
        snprintf(labels[n], sizeof(labels[n]), "%s", v->title ? v->title : v->id);
        rows[n] = labels[n];
        n++;
    }
    nema_strlcpy(A.row_id[n], "athena", sizeof(A.row_id[n]));
    snprintf(labels[n], sizeof(labels[n]), "%s   Home", LV_SYMBOL_HOME);
    rows[n] = labels[n];
    n++;
    sheet_open(SHEET_RECENTS, "Recent apps", rows, n, false);
}

static void sheet_choose(void) {
    int i = A.row_sel;
    if (A.kind == SHEET_KEYBOARD) {
        sheet_close();
        if (i == 0) pal_vk_open();
        else if (i == 1) open_shortcuts_sheet();
        else if (i == 2) pal_desktop_open("settings");
    } else if (A.kind == SHEET_RECENTS) {
        sheet_close();
        pal_desktop_open(A.row_id[i]);
    }
}

static void sheet_move(int d) {
    if (A.row_info || A.nrows == 0) return;
    int old = A.row_sel;
    A.row_sel = (A.row_sel + d + A.nrows) % A.nrows;
    style_row(old);
    style_row(A.row_sel);
}


/* ---- toolkit pages: a bound menu (Settings) drawn in Athena's look ---- */

static pal_menu_t *page_menu(void) {
    if (pal_desktop_overlay_count() || pal_desktop_locked() || pal_toast_showing()) return NULL;
    return pal_menu_bound(pal_desktop_front_id());
}

static bool selectable(const pal_menu_row_t *r) {
    return r->kind != PAL_MENU_SECTION && !r->inert && !(r->kind == PAL_MENU_SLIDER && !r->set);
}

static void page_value(int i) {
    const pal_menu_row_t *r = &A.ppage->rows[A.pidx[i]];
    if (!A.pval[i]) return;
    char v[VALUE_MAX];
    if (r->kind == PAL_MENU_TOGGLE) { /* a switch: green when on, the knob moves */
        bool on = r->get && r->get(r->ctx);
        snprintf(v, sizeof(v), "%d", on);
        if (!strcmp(v, A.ptext[i])) return;
        nema_strlcpy(A.ptext[i], v, sizeof(A.ptext[i]));
        lv_obj_set_style_bg_color(A.pval[i], on ? C_GREEN : C_TRACK, 0);
        lv_obj_t *knob = lv_obj_get_child(A.pval[i], 0);
        lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
        return;
    }
    pal_menu_value_text(r, v, sizeof(v));
    if (r->kind == PAL_MENU_SUBMENU || (r->kind == PAL_MENU_CHOICE)) {
        size_t n = strlen(v);
        snprintf(v + n, sizeof(v) - n, "%s%s", n ? "  " : "", LV_SYMBOL_RIGHT);
    }
    if (!strcmp(v, A.ptext[i])) return;
    nema_strlcpy(A.ptext[i], v, sizeof(A.ptext[i]));
    lv_label_set_text(A.pval[i], v);
}

static void page_style_row(int i) {
    const pal_menu_row_t *r = &A.ppage->rows[A.pidx[i]];
    if (r->kind == PAL_MENU_SECTION) return;
    bool on = A.pidx[i] == A.psel;
    lv_obj_set_style_bg_color(A.prow[i], on ? C_BLUE : C_ROW, 0);
    lv_obj_t *label = lv_obj_get_child(A.prow[i], 0);
    lv_color_t ink = r->danger ? C_RED : r->disabled ? C_GREY : C_INK;
    lv_obj_set_style_text_color(label, on ? C_WHITE : ink, 0);
    if (A.pval[i] && r->kind != PAL_MENU_TOGGLE) lv_obj_set_style_text_color(A.pval[i], on ? C_WHITE : C_GREY, 0);
}

static void page_build(void) {
    lv_obj_clean(A.page_list);
    A.pn = 0;
    A.psel = A.ppage->sel;
    A.prows = A.ppage->n;
    const char *title = A.ppage->title && A.ppage->title[0] ? A.ppage->title : NULL;
    if (!title) { /* a root page without a title: the app's name */
        pal_view_t *fv = pal_desktop_view(pal_desktop_front_id());
        title = fv && fv->title ? fv->title : "";
    }
    lv_label_set_text_fmt(A.page_title, "%s%s", pal_menu_depth(A.pm) > 1 ? LV_SYMBOL_LEFT "  " : "", title);
    int y = 2, w = A.w - 20;
    for (size_t k = 0; k < A.ppage->n && A.pn < MAX_PAGE_ROWS; k++) {
        const pal_menu_row_t *r = &A.ppage->rows[k];
        if (r->hidden) continue;
        int i = A.pn++;
        A.pidx[i] = (int)k;
        A.pval[i] = NULL;
        A.ptext[i][0] = 0;
        lv_obj_t *row = lv_obj_create(A.page_list);
        lv_obj_remove_style_all(row);
        A.prow[i] = row;
        if (r->kind == PAL_MENU_SECTION) { /* a small grey header above its group */
            lv_obj_set_size(row, w, 18);
            lv_obj_set_pos(row, 10, y + 2);
            lv_obj_t *l = text(row, &lv_font_montserrat_12, C_GREY);
            lv_label_set_text(l, r->label ? r->label : "");
            lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 6, 0);
            y += 22;
            continue;
        }
        lv_obj_set_size(row, w, 26);
        lv_obj_set_pos(row, 10, y);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_t *l = text(row, &lv_font_montserrat_16, C_INK);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, w / 2 + 30);
        lv_label_set_text(l, r->label ? r->label : "");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        if (r->kind == PAL_MENU_TOGGLE) {
            lv_obj_t *sw = lv_obj_create(row);
            lv_obj_remove_style_all(sw);
            lv_obj_set_size(sw, 34, 18);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_t *knob = lv_obj_create(sw);
            lv_obj_remove_style_all(knob);
            lv_obj_set_size(knob, 14, 14);
            lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(knob, C_WHITE, 0);
            lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
            A.pval[i] = sw;
        } else {
            lv_obj_t *v = text(row, &lv_font_montserrat_12, C_GREY);
            lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_width(v, w / 2 - 40);
            lv_label_set_text(v, "");
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
            A.pval[i] = v;
        }
        page_value(i);
        page_style_row(i);
        y += 30;
    }
    if (A.pn == 0) lv_label_set_text(A.page_empty, A.ppage->empty ? A.ppage->empty : "Nothing here");
    else lv_label_set_text(A.page_empty, "");
    for (int i = 0; i < A.pn; i++)
        if (A.pidx[i] == A.psel) lv_obj_scroll_to_view(A.prow[i], LV_ANIM_OFF);
}

/* Follows the bound menu: a new page or row count rebuilds, a new selection restyles, values refresh twice a second. */
static void page_sync(pal_menu_t *m, uint32_t now) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p) return;
    if (m != A.pm || p != A.ppage || p->n != A.prows) {
        A.pm = m;
        A.ppage = p;
        page_build();
        A.pvals_ms = now;
        return;
    }
    if (p->sel != A.psel) {
        int old = A.psel;
        A.psel = p->sel;
        for (int i = 0; i < A.pn; i++)
            if (A.pidx[i] == old || A.pidx[i] == A.psel) {
                page_style_row(i);
                if (A.pidx[i] == A.psel) lv_obj_scroll_to_view(A.prow[i], LV_ANIM_OFF);
            }
    }
    if (now - A.pvals_ms >= 500u) {
        A.pvals_ms = now;
        for (int i = 0; i < A.pn; i++) page_value(i);
    }
    (void)selectable;
}

static void build_page(void) {
    A.page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(A.page);
    lv_obj_set_pos(A.page, 0, BAR_H);
    lv_obj_set_size(A.page, A.w, A.h - BAR_H);
    lv_obj_set_style_bg_color(A.page, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(A.page, LV_OBJ_FLAG_SCROLLABLE);
    A.page_title = text(A.page, &lv_font_montserrat_20, C_INK);
    lv_obj_set_pos(A.page_title, 14, 4);
    A.page_list = lv_obj_create(A.page);
    lv_obj_remove_style_all(A.page_list);
    lv_obj_set_pos(A.page_list, 0, 32);
    lv_obj_set_size(A.page_list, A.w, A.h - BAR_H - 32);
    lv_obj_add_flag(A.page_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(A.page_list, LV_SCROLLBAR_MODE_OFF);
    A.page_empty = text(A.page, &lv_font_montserrat_16, C_GREY);
    lv_label_set_text(A.page_empty, "");
    lv_obj_align(A.page_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(A.page, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the chrome ---- */

static bool home_in_front(void) {
    return !strcmp(pal_desktop_front_id(), "athena") && pal_desktop_overlay_count() == 0 && !pal_desktop_locked();
}

static bool owns_screen(void) { return A.disp && (A.kind != SHEET_NONE || (home_in_front() && !pal_toast_showing()) || page_menu()); }

static void ensure(void) {
    if (A.disp) return;
    pal_canvas_t *c = pal_display_canvas();
    A.w = c->w > MAX_W ? MAX_W : c->w;
    A.h = c->h > MAX_H ? MAX_H : c->h;
    lv_init();
    A.disp = lv_display_create(A.w, A.h);
    lv_display_set_color_format(A.disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(A.disp, flush);
    lv_display_set_buffers(A.disp, s_frame, NULL, (uint32_t)A.w * (uint32_t)A.h * 2u, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_WALL_TOP, 0);
    lv_obj_set_style_bg_grad_color(scr, C_WALL_BOT, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_home();
    build_page();
    A.sheet = lv_obj_create(lv_layer_top()); /* created before the bar, so the bar stays above it */
    lv_obj_remove_style_all(A.sheet);
    lv_obj_set_pos(A.sheet, 8, BAR_H + 4);
    lv_obj_set_size(A.sheet, A.w - 16, A.h - BAR_H - 8);
    lv_obj_set_style_radius(A.sheet, 14, 0);
    lv_obj_set_style_bg_color(A.sheet, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(A.sheet, LV_OBJ_FLAG_SCROLLABLE);
    build_bar();
    A.force = true;
}

static bool chrome_key(const pal_key_t *k) {
    if (A.kind != SHEET_NONE) { /* a sheet takes every key while it is open */
        switch (pal_nav_of(k)) {
        case PAL_NAV_UP: sheet_move(-1); break;
        case PAL_NAV_DOWN: sheet_move(1); break;
        case PAL_NAV_OK: sheet_choose(); break;
        case PAL_NAV_BACK:
            if (A.kind == SHEET_SHORTCUTS) open_keyboard_sheet();
            else sheet_close();
            break;
        default:
            if (k->sym == PAL_KEY_VKB || k->sym == PAL_KEY_SWITCH) sheet_close();
            else if (k->sym == PAL_KEY_GOHOME) {
                sheet_close();
                pal_desktop_home();
            }
            break;
        }
        return true;
    }
    if (k->sym == PAL_KEY_VKB) {
        if (pal_vk_is_open()) pal_vk_toggle(); /* Fn twice closes the on-screen keyboard it opened */
        else open_keyboard_sheet();
        return true;
    }
    if (k->sym == PAL_KEY_SWITCH && pal_desktop_overlay_count() == 0) {
        open_recents_sheet();
        return true;
    }
    if (k->sym == PAL_KEY_GOHOME) {
        pal_desktop_home();
        return true;
    }
    return false;
}

static void send(int x0, int y0, int x1, int y1) {
    if (x1 < x0 || y1 < y0) return;
    pal_rect_t r = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    pal_display_present_rgb565(s_frame, A.w, r);
}

static void chrome_frame(uint32_t now) {
    ensure();
    lv_tick_inc(A.last_ms ? now - A.last_ms : 0);
    A.last_ms = now;
    bar_update(false, now);
    pal_tm_t t;
    pal_time_local(&t);
    if (t.hour * 60 + t.minute != A.last_minute) update_clock();
    pal_menu_t *pm = page_menu();
    int mode = home_in_front() ? 0 : pm ? 1 : 2;
    if (mode != A.mode) { /* the home and the page share the screen: show one */
        A.mode = mode;
        if (mode == 1) {
            lv_obj_remove_flag(A.page, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(A.page); /* the home is rebuilt on every return, which puts it on top */
            lv_obj_add_flag(A.home, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(A.page, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(A.home, LV_OBJ_FLAG_HIDDEN);
        }
        A.pm = NULL; /* rebuild the page when it shows again */
        A.force = true;
    }
    if (pm) page_sync(pm, now);
    bool owned = owns_screen();
    bar_style(!home_in_front());
    bool on = pal_dpm_display_on();
    if (owned != A.owned || on != A.display_on) A.force = true;
    A.owned = owned;
    A.display_on = on;
    if (A.force) lv_obj_invalidate(lv_screen_active());
    lv_timer_handler();
    if (!on) return;
    if (owned && A.kind != SHEET_NONE && !home_in_front()) {
        /* a sheet over an app: only the bar and the sheet are ours; the app stays on the panel around it */
        if (A.force || A.pending) {
            send(0, 0, A.w - 1, BAR_H - 1);
            send(8, BAR_H + 4, A.w - 9, A.h - 5);
        }
    } else if (owned) {
        if (A.force) send(0, 0, A.w - 1, A.h - 1);
        else if (A.pending) send(A.dx0, A.dy0, A.dx1, A.dy1);
    } else if (A.force || (A.pending && A.dy0 < BAR_H)) {
        send(0, 0, A.w - 1, BAR_H - 1); /* an app is in front: only the bar is ours */
    }
    A.pending = false;
    A.force = false;
}

static int32_t chrome_next(uint32_t now) {
    (void)now;
    return A.disp ? 33 : 0;
}

static const pal_desktop_chrome_t CHROME = {.hide_bar = true, .hide_legend = true, .top_px = BAR_H, .key = chrome_key,
                                            .owns_screen = owns_screen, .frame = chrome_frame, .next_ms = chrome_next};

/* ---- the home view (the compositor's front view while Athena is home) ---- */

static void enter(pal_view_t *v) {
    (void)v;
    if (A.disp) {
        build_home(); /* the launcher's apps may have changed */
        A.force = true;
    }
}

/* Only while something covers the home (a toast, the on-screen keyboard): copy the home into the 8-bit canvas. */
static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t area, bool full) {
    (void)v, (void)full;
    if (!A.disp) return;
    for (int y = area.y; y < area.y + area.h && y < A.h; y++)
        for (int x = area.x; x < area.x + area.w && x < A.w; x++) c->px[y * c->stride + x] = to_index(s_frame[y * A.w + x], x, y);
    pal_mark(c, area.x, area.y, area.w, area.h);
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
        break;
    case PAL_NAV_BACK: break; /* home has nowhere to go back to */
    default: return false;
    }
    return true;
}

static pal_view_t ATHENA_VIEW = {.id = "athena", .title = "Athena", .flags = PAL_VIEW_HIDDEN, .enter = enter, .draw = draw, .key = key};

static bool s_view_registered;

static bool athena_start(void) {
    if (!pal_display_has_rgb565()) return false; /* Athena draws in full colour; a panel driver without it keeps classic */
    if (!s_view_registered) {
        nema_err_t r = pal_desktop_register(&ATHENA_VIEW);
        if (r != NEMA_OK && r != NEMA_ERR_EXIST) return false;
        s_view_registered = true;
    }
    pal_desktop_set_chrome(&CHROME);
    pal_desktop_set_home("athena");
    /* the home uses the arrows and Enter itself: the classic home keys go away (Settings, Files and the rest are on the home) */
    pal_desktop_bind_home_key(PAL_KEY_ENTER, NULL);
    pal_desktop_bind_home_key(PAL_KEY_UP, NULL);
    pal_desktop_bind_home_key(PAL_KEY_DOWN, NULL);
    A.force = true;
    return true;
}

static void athena_stop(void) {
    if (A.disp) sheet_close();
}

static const pal_desktop_env_t ATHENA = {.id = "athena", .title = "Athena", .about = "full-colour LVGL desktop: own bar, keys and home",
                                         .start = athena_start, .stop = athena_stop};

void pal_athena_register(void) { (void)pal_desktops_register(&ATHENA); }
