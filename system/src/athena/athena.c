/* Athena: a desktop session drawn with LVGL in full colour (docs/DESKTOPS.md, ADR-0010).
 *
 * Athena owns its chrome (pal_desktop_chrome_t): its own status bar on top of every screen, its own global keys (Fn twice opens its
 * Keyboard sheet, a tap of the cycle key its Recent apps sheet, holding it goes home), its own home and lock screen, and it draws the
 * shell services and toasts itself (athena_shell.c). LVGL renders into Athena's own RGB565 frame (direct mode: the frame persists,
 * only dirty areas are redrawn) and Athena copies what is its own into the system's screen frame (pal_display_present_rgb565).
 *
 * While the home, a sheet, a modal, the lock screen or a bound menu is in front, Athena owns the whole screen and the compositor draws
 * nothing. While an app is in front, the compositor draws it below Athena's bar (top_px rows are kept for the bar) and Athena sends
 * only its bar (and its softkey bar, for a theme with one). Parts: athena_lock.c (lock screen), athena_page.c (bound menus),
 * athena_shell.c (services, toasts), athena_softkeys.c (the softkey bar), athena_theme.c (themes), athena_widgets.c (home widgets). */
#include "athena_int.h"
#include "pal/shell.h"
#include "athena_pool.h"

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

athena_t A NEMA_EXT_BSS;

uint16_t *s_frame; /* the LVGL frame, taken at the start */
static void *s_pool;      /* LVGL's pool, taken at the start */
static nema_owner_t s_owner;

void *athena_pool(size_t size) { return size <= LV_MEM_SIZE ? s_pool : NULL; }

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
    if (ath_T->level_bars) { /* the signal and the battery are bars: lit up to their level */
        for (int i = 0; i < 4; i++) lv_obj_set_style_bg_opa(A.lvl_sig[i], i < st.wifi ? LV_OPA_COVER : LV_OPA_30, 0);
        int lit = st.battery < 0 ? 0 : (st.battery + 19) / 20;
        for (int i = 0; i < 5; i++) lv_obj_set_style_bg_opa(A.lvl_bat[i], i < lit ? LV_OPA_COVER : LV_OPA_30, 0);
    } else {
        if (st.wifi > 0) n += snprintf(right + n, sizeof(right) - (size_t)n, "%s  ", LV_SYMBOL_WIFI);
        if (st.battery >= 0) snprintf(right + n, sizeof(right) - (size_t)n, "%d%%  %s", st.battery, battery_glyph(st.battery));
    }
    lv_label_set_text(A.bar_right, right);
}

/* Rising bars at x (left to right), the tallest on the side away from the screen's edge. */
static void level_bars(lv_obj_t **out, int n, int x, bool rise_right) {
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_create(A.bar);
        lv_obj_remove_style_all(b);
        int hh = 4 + (rise_right ? i : n - 1 - i) * 10 / n;
        lv_obj_set_size(b, 3, hh);
        lv_obj_set_pos(b, x + i * 5, BAR_H - 4 - hh);
        lv_obj_set_style_bg_color(b, C_WHITE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
        out[i] = b;
    }
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
    lv_obj_set_style_bg_color(A.bar, lv_color_hex(ath_T->bar_bg), 0);
    lv_obj_set_style_bg_opa(A.bar, LV_OPA_TRANSP, 0);
    A.bar_solid = false;
    A.bar_time = lv_label_create(A.bar);
    lv_obj_set_style_text_font(A.bar_time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_time, C_WHITE, 0);
    if (ath_T->level_bars) { /* the signal on the left, the battery on the right, the clock between them */
        level_bars(A.lvl_sig, 4, 8, true);
        level_bars(A.lvl_bat, 5, A.w - 8 - 5 * 5 + 2, false);
        lv_obj_align(A.bar_time, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_align(A.bar_time, LV_ALIGN_LEFT_MID, 12, 0);
    }
    A.bar_toast = lv_label_create(A.bar); /* a toast takes the bar while it shows (athena_shell.c) */
    lv_obj_set_style_text_font(A.bar_toast, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_toast, C_WHITE, 0);
    lv_label_set_long_mode(A.bar_toast, LV_LABEL_LONG_DOT);
    lv_obj_set_width(A.bar_toast, A.w - 24);
    lv_obj_set_style_text_align(A.bar_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(A.bar_toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(A.bar_toast, LV_OBJ_FLAG_HIDDEN);
    A.bar_right = lv_label_create(A.bar);
    lv_obj_set_style_text_font(A.bar_right, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_right, C_WHITE, 0);
    lv_obj_align(A.bar_right, LV_ALIGN_RIGHT_MID, ath_T->level_bars ? -40 : -10, 0);
    bar_update(true, 0);
}

/* ---- home ---- */

void ath_focus_bg(lv_obj_t *o, bool on, lv_color_t off) {
    bool grad = on && ath_T->hl_top != ath_T->hl_bot;
    lv_obj_set_style_bg_color(o, grad ? lv_color_hex(ath_T->hl_top) : on ? C_BLUE : off, 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(ath_T->hl_bot), 0);
    lv_obj_set_style_bg_grad_dir(o, grad ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE, 0);
}

static void style_tile(int i) {
    bool on = i == A.sel;
    if (ath_T->idle_home) { /* the menu grid: the highlight fills the cell behind the icon and the name */
        lv_obj_set_style_bg_opa(A.tile[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        ath_focus_bg(A.tile[i], on, C_ROW);
        lv_obj_set_style_text_color(lv_obj_get_child(A.tile[i], 1), on ? C_WHITE : C_INK, 0);
        return;
    }
    lv_obj_t *icon = lv_obj_get_child(A.tile[i], 0);
    lv_obj_set_style_outline_width(icon, on ? 2 : 0, 0);
    lv_obj_set_style_outline_color(icon, C_WHITE, 0);
    lv_obj_set_style_outline_pad(icon, 2, 0);
    lv_obj_set_style_outline_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(icon, on ? 272 : 256, 0); /* 256 is 100%; it grows from its centre (the pivot, build_home) */
    lv_obj_set_style_text_opa(lv_obj_get_child(A.tile[i], 1), on ? LV_OPA_COVER : LV_OPA_70, 0);
}

static void select_tile(int i) {
    if (A.n == 0) return;
    int old = A.sel;
    A.sel = i < 0 ? 0 : i >= A.n ? A.n - 1 : i;
    style_tile(old);
    style_tile(A.sel);
    int top_row = A.n ? A.trow[0] : 0;
    if (A.trow[A.sel] == top_row) lv_obj_scroll_to_y(A.grid, 0, LV_ANIM_OFF); /* the first row of apps: the widgets above stay in view */
    else lv_obj_scroll_to_view(A.tile[A.sel], LV_ANIM_OFF);
}

/* Once a second (and at once after a build): the widgets follow the clock. */
static void widgets_tick(bool now) {
    static int last = -1;
    pal_tm_t t;
    pal_time_local(&t);
    if (!now && t.second == last) return;
    last = t.second;
    for (int i = 0; i < A.nw; i++)
        if (A.wdef[i]->tick) A.wdef[i]->tick(A.wbox[i], &t);
}

lv_obj_t *ath_text(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

/* The home below the bar: a grid that follows the screen. The edge padding is a fortieth of the width, the columns as many cells of
 * at least 60 pixels as fit (3 to 8), a cell 9/8 as tall as it is wide, the gap between widgets and the icon size in proportion
 * (5 cells of 60 x 67 with 8 px edges on 320 pixels). The widgets come first and take their spans (the clock 2 x 2, the calendar
 * 3 x 2), then the launcher's apps one cell each, every item in the first free place reading left to right. The grid scrolls when
 * it is taller than the screen; the arrows move between the apps by their place in it. */
#define GRID_ROWS 16

static bool place(bool used[GRID_ROWS][8], int cols, int span_c, int span_r, int *pc, int *pr) {
    for (int r = 0; r + span_r <= GRID_ROWS; r++)
        for (int c = 0; c + span_c <= cols; c++) {
            bool free = true;
            for (int y = r; y < r + span_r && free; y++)
                for (int x = c; x < c + span_c && free; x++) free = !used[y][x];
            if (!free) continue;
            for (int y = r; y < r + span_r; y++)
                for (int x = c; x < c + span_c; x++) used[y][x] = true;
            *pc = c, *pr = r;
            return true;
        }
    return false;
}

static lv_obj_t *build_widget(lv_obj_t *parent, const ath_widget_t *wd, int x, int y, int bw, int bh) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, bw, bh);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    wd->build(box, bw, bh);
    A.wbox[A.nw] = box;
    A.wdef[A.nw] = wd;
    A.nw++;
    return box;
}

static void build_tile(lv_obj_t *parent, int c, int r, int x, int y, int cell_w, int cell_h, int icon_px, int icon_y);

/* The idle home (a theme with idle_home): the widgets one above the other across the screen, each as tall as its rows ask; and the app
 * menu the left softkey or Enter opens, a title pane over a grid of three columns that scrolls. */
static void build_idle_home(int w, int h) {
    int pad = w / 40 < 4 ? 4 : w / 40, total = 0;
    const ath_widget_t *wds[MAX_WIDGETS];
    int nwd = 0;
    for (int k = 0; ATH_HOME_WIDGETS[k] && nwd < MAX_WIDGETS; k++) {
        const ath_widget_t *wd = ath_widget_find(ATH_HOME_WIDGETS[k]);
        if (wd) wds[nwd++] = wd, total += wd->rows;
    }
    A.nw = 0;
    int y = pad, avail = h - pad * (nwd + 1);
    for (int i = 0; i < nwd; i++) {
        int bh = total ? avail * wds[i]->rows / total : 0;
        build_widget(A.home, wds[i], pad, y, w - 2 * pad, bh);
        y += bh + pad;
    }

    A.menu = lv_obj_create(A.home);
    lv_obj_remove_style_all(A.menu);
    lv_obj_set_size(A.menu, w, h);
    lv_obj_set_style_bg_color(A.menu, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.menu, LV_OPA_COVER, 0);
    lv_obj_remove_flag(A.menu, LV_OBJ_FLAG_SCROLLABLE);
    const int PANE = 24;
    lv_obj_t *pane = lv_obj_create(A.menu);
    lv_obj_remove_style_all(pane);
    lv_obj_set_size(pane, w, PANE);
    ath_pane_bg(pane);
    lv_obj_t *t = ath_text(pane, &lv_font_montserrat_16, C_WHITE);
    lv_label_set_text(t, LV_SYMBOL_LIST "  Menu");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);
    A.cols = 3;
    const int CELL_W = (w - 2 * pad) / A.cols, CELL_H = (h - PANE) / 3, ICON = CELL_H * 9 / 16;
    A.grid = lv_obj_create(A.menu);
    lv_obj_remove_style_all(A.grid);
    lv_obj_set_pos(A.grid, 0, PANE);
    lv_obj_set_size(A.grid, w, h - PANE);
    lv_obj_add_flag(A.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(A.grid, LV_SCROLLBAR_MODE_OFF);
    bool used[GRID_ROWS][8];
    memset(used, 0, sizeof(used));
    A.n = 0;
    for (size_t i = 0; i < psys_launcher_count() && A.n < MAX_TILES; i++) {
        if (!psys_launcher_get(i, &A.app[A.n])) continue;
        int c, r;
        if (!place(used, A.cols, 1, 1, &c, &r)) break;
        build_tile(A.grid, c, r, pad + c * CELL_W + 2, r * CELL_H + 2, CELL_W - 4, CELL_H - 4, ICON, CELL_H / 12);
        A.n++;
    }
    if (!A.menu_open) lv_obj_add_flag(A.menu, LV_OBJ_FLAG_HIDDEN);
}

/* One app on the home's grid at column c, row r: a cell (child 0 the icon, child 1 the name). */
static void build_tile(lv_obj_t *parent, int c, int r, int x, int y, int cell_w, int cell_h, int icon_px, int icon_y) {
    A.tcol[A.n] = (uint8_t)c, A.trow[A.n] = (uint8_t)r;
    const icon_style_t *st = icon_for(A.app[A.n].id);
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, cell_w, cell_h);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_radius(t, ath_T->r_row, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    A.tile[A.n] = t;
    lv_obj_t *icon = lv_obj_create(t);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, icon_px, icon_px);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, icon_y);
    lv_obj_set_style_transform_pivot_x(icon, icon_px / 2, 0);
    lv_obj_set_style_transform_pivot_y(icon, icon_px / 2, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_OVERFLOW_VISIBLE); /* the ring may reach past the cell's edge */
    lv_obj_set_style_radius(icon, ath_T->r_icon, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(st->top), 0);
    lv_obj_set_style_bg_grad_color(icon, lv_color_hex(st->bot), 0);
    lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(icon, ath_T->flat ? 0 : 8, 0);
    lv_obj_set_style_shadow_offset_y(icon, 2, 0);
    lv_obj_set_style_shadow_opa(icon, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(icon, C_BLACK, 0);
    lv_obj_t *g = ath_text(icon, icon_px >= 32 ? &lv_font_montserrat_20 : &lv_font_montserrat_16, C_WHITE);
    if (st->glyph) lv_label_set_text(g, st->glyph);
    else {
        char init[2] = {A.app[A.n].name[0] ? A.app[A.n].name[0] : '?', 0};
        lv_label_set_text(g, init);
    }
    lv_obj_center(g);
    lv_obj_t *name = ath_text(t, &lv_font_montserrat_12, C_WHITE);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, cell_w - 4, 14); /* one line: a longer name ends in dots */
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name, A.app[A.n].name);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, ath_T->idle_home ? -2 : 0);
}

static void build_home(void) {
    int w = A.w, h = A.h - BAR_H - ath_bottom();
    if (A.home) lv_obj_delete(A.home);
    A.home = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(A.home);
    lv_obj_set_pos(A.home, 0, BAR_H);
    lv_obj_set_size(A.home, w, h);
    lv_obj_remove_flag(A.home, LV_OBJ_FLAG_SCROLLABLE);
    A.menu = NULL;
    if (ath_T->idle_home) {
        build_idle_home(w, h);
        if (A.sel >= A.n) A.sel = 0;
        for (int i = 0; i < A.n; i++) style_tile(i);
        widgets_tick(true);
        return;
    }

    int pad = w / 40, cols = (w - 2 * pad) / 60;
    cols = cols < 3 ? 3 : cols > 8 ? 8 : cols;
    const int CELL_W = (w - 2 * pad) / cols, CELL_H = CELL_W * 9 / 8, GAP = CELL_W / 12 < 3 ? 3 : CELL_W / 12;
    /* the icon and the room for its focus ring (a 2 px outline 2 px out, on a 6% larger icon) above it; the name under it */
    const int ICON = CELL_W * 5 / 8, ICON_Y = CELL_H / 10;
    A.grid = lv_obj_create(A.home);
    lv_obj_remove_style_all(A.grid);
    lv_obj_set_pos(A.grid, 0, 0);
    lv_obj_set_size(A.grid, w, h);
    lv_obj_add_flag(A.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(A.grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(A.grid, pad, 0);
    A.cols = cols;
    int margin = (w - A.cols * CELL_W) / 2;
    bool used[GRID_ROWS][8];
    memset(used, 0, sizeof(used));
    A.nw = 0;
    for (int k = 0; ATH_HOME_WIDGETS[k] && A.nw < MAX_WIDGETS; k++) {
        const ath_widget_t *wd = ath_widget_find(ATH_HOME_WIDGETS[k]);
        int c, r;
        if (!wd || wd->cols > A.cols || !place(used, A.cols, wd->cols, wd->rows, &c, &r)) continue;
        build_widget(A.grid, wd, margin + c * CELL_W + GAP / 2, r * CELL_H + GAP / 2, wd->cols * CELL_W - GAP, wd->rows * CELL_H - GAP);
    }
    A.n = 0;
    for (size_t i = 0; i < psys_launcher_count() && A.n < MAX_TILES; i++) {
        if (!psys_launcher_get(i, &A.app[A.n])) continue;
        int c, r;
        if (!place(used, A.cols, 1, 1, &c, &r)) break;
        build_tile(A.grid, c, r, margin + c * CELL_W, r * CELL_H, CELL_W, CELL_H, ICON, ICON_Y);
        A.n++;
    }
    if (A.sel >= A.n) A.sel = 0;
    for (int i = 0; i < A.n; i++) style_tile(i);
    widgets_tick(true);
}

/* The arrows on the home: left and right go through the apps in order; up and down to the nearest app in the next row that has one. */
static void home_move(int dir_row) {
    if (A.n == 0) return;
    int r0 = A.trow[A.sel], c0 = A.tcol[A.sel], best = -1, best_r = 0, best_d = 0;
    for (int i = 0; i < A.n; i++) {
        int dr = (A.trow[i] - r0) * dir_row;
        if (dr <= 0) continue;
        int dc = A.tcol[i] > c0 ? A.tcol[i] - c0 : c0 - A.tcol[i];
        if (best < 0 || dr < best_r || (dr == best_r && dc < best_d)) best = i, best_r = dr, best_d = dc;
    }
    if (best >= 0) select_tile(best);
    else if (dir_row < 0) lv_obj_scroll_to_y(A.grid, 0, LV_ANIM_OFF); /* above the first apps: the widgets */
}

/* The idle home's app menu: open over the idle screen, or closed back to it. */
void ath_menu_show(bool open) {
    A.menu_open = open && A.menu;
    if (!A.menu) return;
    if (A.menu_open) lv_obj_remove_flag(A.menu, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(A.menu, LV_OBJ_FLAG_HIDDEN);
    A.force = true;
}

/* ---- the chrome ---- */

bool ath_home_in_front(void) {
    return !strcmp(pal_desktop_front_id(), "athena") && pal_desktop_overlay_count() == 0 && !pal_desktop_locked();
}

static bool owns_screen(void) {
    /* a shell service in front is Athena's to draw (athena_shell.c): asked here, before the compositor's frame, so it never draws one */
    return A.disp && (pal_desktop_locked() || A.kind != SHEET_NONE || pal_shell_top(NULL) || ath_home_in_front() || ath_page_menu());
}

static void build_all(void);
static void chrome_apply(void);

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
    ath_T = ath_theme_wanted();
    build_all();
}

/* Everything Athena shows, in the theme of the moment. The order is the stacking order on the top layer. */
static void build_all(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_WALL_TOP, 0);
    lv_obj_set_style_bg_grad_color(scr, C_WALL_BOT, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_home();
    ath_build_page();
    A.sheet = lv_obj_create(lv_layer_top()); /* created before the bar, so the bar stays above it */
    lv_obj_remove_style_all(A.sheet);
    ath_sheet_geometry();
    lv_obj_set_style_radius(A.sheet, ath_T->r_card, 0);
    lv_obj_set_style_bg_color(A.sheet, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(A.sheet, LV_OBJ_FLAG_SCROLLABLE);
    ath_shell_build(); /* the modal for the shell services: above the sheet */
    ath_build_lock();  /* above the modal, below the bar */
    build_bar();
    ath_softkeys_build(); /* last: above everything, as the bar */
    A.force = true;
}

/* A new theme: every object goes and comes back in the new look; what is open (a sheet, a modal) is built again on the next frame. */
static void theme_follow(void) {
    const ath_theme_t *want = ath_theme_wanted();
    if (want == ath_T) return;
    ath_T = want;
    lv_obj_clean(lv_layer_top());
    lv_obj_clean(lv_screen_active());
    A.home = A.page = A.sheet = A.lock = A.backdrop = A.dim = A.modal = A.bar = A.menu = A.sk = NULL;
    A.menu_open = false;
    A.kind = SHEET_NONE;
    A.svc_kind = 0, A.svc_sig = 0;
    A.lock_shown = A.toast_shown = false;
    A.pm = NULL;
    A.mode = -1;
    A.bar_solid = false;
    build_all();
    bar_update(true, 0);
    chrome_apply();
}

static bool chrome_key(const pal_key_t *k) {
    if (A.kind != SHEET_NONE) { /* a sheet takes every key while it is open */
        switch (pal_nav_of(k)) {
        case PAL_NAV_UP: ath_sheet_move(-1); break;
        case PAL_NAV_DOWN: ath_sheet_move(1); break;
        case PAL_NAV_OK: ath_sheet_choose(); break;
        case PAL_NAV_BACK:
            if (A.kind == SHEET_SHORTCUTS) ath_open_keyboard_sheet();
            else ath_sheet_close();
            break;
        default:
            if (k->sym == PAL_KEY_VKB || k->sym == PAL_KEY_SWITCH || (k->sym == PAL_KEY_TAB && A.kind == SHEET_OPTIONS)) ath_sheet_close();
            else if (k->sym == PAL_KEY_GOHOME) {
                ath_sheet_close();
                pal_desktop_home();
            }
            break;
        }
        return true;
    }
    if (k->sym == PAL_KEY_VKB) {
        if (pal_vk_is_open()) pal_vk_toggle(); /* Fn twice closes the on-screen keyboard it opened */
        else ath_open_keyboard_sheet();
        return true;
    }
    if (k->sym == PAL_KEY_SWITCH && pal_desktop_overlay_count() == 0) {
        ath_open_recents_sheet();
        return true;
    }
    if (k->sym == PAL_KEY_GOHOME) {
        ath_menu_show(false);
        pal_desktop_home();
        return true;
    }
    /* the left softkey (Tab) on a screen Athena draws: Menu on the idle screen, Options everywhere else; an app keeps its Tab */
    if (ath_T->softkeys && k->sym == PAL_KEY_TAB && !pal_shell_top(NULL) && !pal_desktop_locked() &&
        (ath_home_in_front() || ath_page_menu())) {
        if (ath_home_in_front() && !A.menu_open) ath_menu_show(true);
        else ath_open_options();
        return true;
    }
    return false;
}

/* The softkeys' labels follow what is in front. */
static void softkeys_follow(void) {
    if (!A.sk) return;
    ath_softkeys_show(!pal_desktop_locked());
    pal_menu_t *pm = ath_page_menu();
    if (A.kind == SHEET_OPTIONS) ath_softkeys_set("Select", "", "Cancel");
    else if (A.kind != SHEET_NONE) ath_softkeys_set(A.row_info ? "" : "Select", "", "Back");
    else if (ath_shell_active()) ath_softkeys_set("", "OK", "Cancel");
    else if (ath_home_in_front()) {
        if (A.menu_open) ath_softkeys_set("Options", A.n ? "Open" : "", "Back");
        else ath_softkeys_set("Menu", "", "Recent");
    } else if (pm) {
        pal_menu_page_t *top = pal_menu_top(pm);
        ath_softkeys_set("Options", top && pal_menu_has_selectable(top) ? "Select" : "", pal_menu_depth(pm) > 1 ? "Back" : "Exit");
    } else {
        ath_softkeys_set("", "", "Back");
    }
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
    static uint32_t theme_at;
    if (now - theme_at >= 1000u) { /* the setting is read once a second, as the bar's data is */
        theme_at = now;
        theme_follow();
    }
    bar_update(false, now);
    pal_tm_t t;
    pal_time_local(&t);
    if (t.hour * 60 + t.minute != A.last_minute) {
        A.last_minute = t.hour * 60 + t.minute;
        A.lock_dirty = true;
    }
    if (ath_home_in_front()) widgets_tick(false);
    pal_menu_t *pm = ath_page_menu();
    int mode = ath_home_in_front() ? 0 : pm ? 1 : 2;
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
    if (pm) ath_page_sync(pm, now);
    ath_lock_sync();
    ath_shell_sync();
    softkeys_follow();
    bool owned = owns_screen();
    bar_style(!ath_home_in_front());
    bool on = pal_dpm_display_on();
    if (owned != A.owned || on != A.display_on || pal_console_generation() != A.console_gen) A.force = true; /* the console had the screen */
    A.console_gen = pal_console_generation();
    A.owned = owned;
    A.display_on = on;
    if (A.force) lv_obj_invalidate(lv_screen_active());
    lv_timer_handler();
    if (!on) return;
    if (owned && A.kind != SHEET_NONE && !ath_home_in_front()) {
        /* a sheet over an app: only the bar and the sheet are ours; the app stays on the panel around it */
        if (A.force || A.pending) {
            send(0, 0, A.w - 1, BAR_H - 1);
            send(8, BAR_H + 4, A.w - 9, A.h - 5 - ath_bottom());
            if (ath_bottom()) send(0, A.h - SK_H, A.w - 1, A.h - 1);
        }
    } else if (owned) {
        if (A.force) send(0, 0, A.w - 1, A.h - 1);
        else if (A.pending) send(A.dx0, A.dy0, A.dx1, A.dy1);
    } else {
        /* an app is in front: only the bar (and the softkey bar) are ours */
        if (A.force || (A.pending && A.dy0 < BAR_H)) send(0, 0, A.w - 1, BAR_H - 1);
        if (ath_bottom() && (A.force || (A.pending && A.dy1 >= A.h - SK_H))) send(0, A.h - SK_H, A.w - 1, A.h - 1);
    }
    A.pending = false;
    A.force = false;
}

static int32_t chrome_next(uint32_t now) {
    (void)now;
    return A.disp ? 33 : 0;
}

static const pal_desktop_chrome_t CHROME = {.hide_bar = true, .hide_legend = true, .top_px = BAR_H, .key = chrome_key, .services = true,
                                            .owns_screen = owns_screen, .frame = chrome_frame, .next_ms = chrome_next};
/* The same with the softkey bar's rows kept at the bottom: an app stops above it. */
static const pal_desktop_chrome_t CHROME_SK = {.hide_bar = true, .hide_legend = true, .top_px = BAR_H, .bottom_px = SK_H, .key = chrome_key,
                                               .services = true, .owns_screen = owns_screen, .frame = chrome_frame, .next_ms = chrome_next};

static void chrome_apply(void) { pal_desktop_set_chrome(ath_T->softkeys ? &CHROME_SK : &CHROME); }

/* ---- the home view (the compositor's front view while Athena is home) ---- */

static void enter(pal_view_t *v) {
    (void)v;
    A.menu_open = false; /* coming home is coming to the idle screen */
    if (A.disp) {
        build_home(); /* the launcher's apps may have changed */
        A.force = true;
    }
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (A.menu && !A.menu_open) { /* the idle screen: Enter opens the menu, Esc the recent apps */
        switch (pal_nav_of(k)) {
        case PAL_NAV_OK: ath_menu_show(true); return true;
        case PAL_NAV_BACK: ath_open_recents_sheet(); return true;
        case PAL_NAV_UP: case PAL_NAV_DOWN: case PAL_NAV_LEFT: case PAL_NAV_RIGHT: return true;
        default: return false;
        }
    }
    if (A.menu && pal_nav_of(k) == PAL_NAV_BACK) {
        ath_menu_show(false);
        return true;
    }
    switch (pal_nav_of(k)) {
    case PAL_NAV_LEFT: select_tile(A.sel - 1); break;
    case PAL_NAV_RIGHT: select_tile(A.sel + 1); break;
    case PAL_NAV_UP: home_move(-1); break;
    case PAL_NAV_DOWN: home_move(1); break;
    case PAL_NAV_OK:
        if (A.n > 0) pal_launcher_run(&A.app[A.sel]);
        break;
    case PAL_NAV_BACK: break; /* home has nowhere to go back to */
    default: return false;
    }
    return true;
}

/* The home is always Athena's to draw (it owns the screen while the home is in front), so the view draws nothing itself. */
static pal_view_t ATHENA_VIEW = {.id = "athena", .title = "Athena", .flags = PAL_VIEW_HIDDEN, .enter = enter, .key = key};

/* The session's start takes the pool and the frame and registers its views (they are its own: its stop takes them away). LVGL
 * starts on the first frame. Its stop shuts LVGL down and gives back all of its memory. */
static void athena_stop(void);

static bool athena_start(void) {
    if (!pal_display_has_rgb565()) return false; /* Athena draws in full colour */
    if (!s_owner) s_owner = nema_owner_register("athena");
    pal_canvas_t *c = pal_display_canvas();
    int w = c->w > MAX_W ? MAX_W : c->w, h = c->h > MAX_H ? MAX_H : c->h;
    s_pool = nema_alloc_ex(NEMA_MEM_EXTERNAL, s_owner, LV_MEM_SIZE, 8, 0);
    s_frame = nema_alloc_ex(NEMA_MEM_EXTERNAL, s_owner, (size_t)w * (size_t)h * 2u, 4, NEMA_ALLOC_ZERO);
    if (!s_pool || !s_frame) {
        nema_free(s_pool), nema_free(s_frame);
        s_pool = NULL, s_frame = NULL;
        return false;
    }
    memset(&A, 0, sizeof(A));
    A.snap = nema_alloc_ex(NEMA_MEM_EXTERNAL, s_owner, (size_t)w * (size_t)h * 2u, 4, 0); /* the screen under a modal */
    if (!A.snap) {
        athena_stop();
        return false;
    }
    if (pal_desktop_register(&ATHENA_VIEW) != NEMA_OK || pal_desktop_register(&ath_lock_view) != NEMA_OK) {
        athena_stop(); /* the registry takes the views it got away again */
        return false;
    }
    ath_T = ath_theme_wanted();
    chrome_apply();
    pal_desktop_set_home("athena");
    pal_desktop_set_lock_view("athena-lock");
    A.force = true;
    return true;
}

static void athena_stop(void) {
    if (A.disp) lv_deinit();
    nema_free(A.snap);
    memset(&A, 0, sizeof(A));
    nema_free(s_pool), nema_free(s_frame);
    s_pool = NULL, s_frame = NULL;
}

const pal_desktop_env_t PAL_DESKTOP_ATHENA = {.id = "athena", .title = "Athena", .about = "full-colour LVGL desktop",
                                            .start = athena_start, .stop = athena_stop};
