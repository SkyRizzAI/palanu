/* Athena: a desktop session drawn with LVGL in full colour (docs/DESKTOPS.md, ADR-0010).
 *
 * Athena owns its chrome (pal_desktop_chrome_t): its own status bar on top of every screen, its own global keys (Fn twice opens its
 * Keyboard sheet, a tap of the cycle key its Recent apps sheet, holding it goes home), its own home and lock screen, and it draws the
 * shell services and toasts itself (athena_shell.c). LVGL renders into Athena's own RGB565 frame (direct mode: the frame persists,
 * only dirty areas are redrawn) and Athena copies what is its own into the system's screen frame (pal_display_present_rgb565).
 *
 * While the home, a sheet, a modal, the lock screen or a bound menu is in front, Athena owns the whole screen and the compositor draws
 * nothing. While an app is in front, the compositor draws it below Athena's bar (top_px rows are kept for the bar) and Athena sends
 * only its bar. Parts: athena_lock.c (lock screen), athena_page.c (bound menus), athena_shell.c (services, toasts). */
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
    lv_obj_set_style_bg_color(A.bar, lv_color_hex(ath_T->bar_bg), 0);
    lv_obj_set_style_bg_opa(A.bar, LV_OPA_TRANSP, 0);
    A.bar_solid = false;
    A.bar_time = lv_label_create(A.bar);
    lv_obj_set_style_text_font(A.bar_time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_time, C_WHITE, 0);
    lv_obj_align(A.bar_time, LV_ALIGN_LEFT_MID, 12, 0);
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

lv_obj_t *ath_text(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
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

    A.date = ath_text(A.home, &lv_font_montserrat_16, C_WHITE);
    lv_obj_set_style_text_opa(A.date, LV_OPA_70, 0);
    lv_obj_align(A.date, LV_ALIGN_TOP_MID, 0, 0);
    A.clock = ath_text(A.home, &lv_font_montserrat_48, C_WHITE);
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
        lv_obj_set_style_radius(icon, ath_T->r_icon, 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(st->top), 0);
        lv_obj_set_style_bg_grad_color(icon, lv_color_hex(st->bot), 0);
        lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_shadow_width(icon, ath_T->flat ? 0 : 8, 0);
        lv_obj_set_style_shadow_offset_y(icon, 2, 0);
        lv_obj_set_style_shadow_opa(icon, LV_OPA_40, 0);
        lv_obj_set_style_shadow_color(icon, C_BLACK, 0);
        lv_obj_t *g = ath_text(icon, &lv_font_montserrat_20, C_WHITE);
        if (st->glyph) lv_label_set_text(g, st->glyph);
        else {
            char init[2] = {A.app[A.n].name[0] ? A.app[A.n].name[0] : '?', 0};
            lv_label_set_text(g, init);
        }
        lv_obj_center(g);
        lv_obj_t *name = ath_text(t, &lv_font_montserrat_12, C_WHITE);
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
    lv_obj_t *hint = ath_text(pill, &lv_font_montserrat_12, C_WHITE);
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
    A.sheet_title = ath_text(A.sheet, &lv_font_montserrat_20, C_INK);
    lv_label_set_text(A.sheet_title, title);
    lv_obj_set_pos(A.sheet_title, 14, 10);
    int rh = info ? 20 : 26, y = 40;
    for (int i = 0; i < A.nrows; i++) {
        lv_obj_t *r = lv_obj_create(A.sheet);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, A.w - 16 - 20, rh);
        lv_obj_set_pos(r, 10, y + i * (rh + 4));
        lv_obj_set_style_radius(r, ath_T->r_row, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_t *l = ath_text(r, info ? &lv_font_montserrat_12 : &lv_font_montserrat_16, C_INK);
        lv_label_set_text(l, rows[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        A.row[i] = r;
        style_row(i);
    }
    A.sheet_hint = ath_text(A.sheet, &lv_font_montserrat_12, C_GREY);
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
    static char labels[MAX_ROWS][40] NEMA_EXT_BSS;
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


/* ---- the chrome ---- */

bool ath_home_in_front(void) {
    return !strcmp(pal_desktop_front_id(), "athena") && pal_desktop_overlay_count() == 0 && !pal_desktop_locked();
}

static bool owns_screen(void) {
    /* a shell service in front is Athena's to draw (athena_shell.c): asked here, before the compositor's frame, so it never draws one */
    return A.disp && (pal_desktop_locked() || A.kind != SHEET_NONE || pal_shell_top(NULL) || ath_home_in_front() || ath_page_menu());
}

static void build_all(void);

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
    lv_obj_set_pos(A.sheet, 8, BAR_H + 4);
    lv_obj_set_size(A.sheet, A.w - 16, A.h - BAR_H - 8);
    lv_obj_set_style_radius(A.sheet, ath_T->r_card, 0);
    lv_obj_set_style_bg_color(A.sheet, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(A.sheet, LV_OBJ_FLAG_SCROLLABLE);
    ath_shell_build(); /* the modal for the shell services: above the sheet */
    ath_build_lock();  /* above the modal, below the bar */
    build_bar();
    A.force = true;
}

/* A new theme: every object goes and comes back in the new look; what is open (a sheet, a modal) is built again on the next frame. */
static void theme_follow(void) {
    const ath_theme_t *want = ath_theme_wanted();
    if (want == ath_T) return;
    ath_T = want;
    lv_obj_clean(lv_layer_top());
    lv_obj_clean(lv_screen_active());
    A.home = A.page = A.sheet = A.lock = A.backdrop = A.dim = A.modal = A.bar = NULL;
    A.kind = SHEET_NONE;
    A.svc_kind = 0, A.svc_sig = 0;
    A.lock_shown = A.toast_shown = false;
    A.pm = NULL;
    A.mode = -1;
    A.bar_solid = false;
    build_all();
    bar_update(true, 0);
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
    static uint32_t theme_at;
    if (now - theme_at >= 1000u) { /* the setting is read once a second, as the bar's data is */
        theme_at = now;
        theme_follow();
    }
    bar_update(false, now);
    pal_tm_t t;
    pal_time_local(&t);
    if (t.hour * 60 + t.minute != A.last_minute) {
        update_clock();
        A.lock_dirty = true;
    }
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

static const pal_desktop_chrome_t CHROME = {.hide_bar = true, .hide_legend = true, .top_px = BAR_H, .key = chrome_key, .services = true,
                                            .owns_screen = owns_screen, .frame = chrome_frame, .next_ms = chrome_next};

/* ---- the home view (the compositor's front view while Athena is home) ---- */

static void enter(pal_view_t *v) {
    (void)v;
    if (A.disp) {
        build_home(); /* the launcher's apps may have changed */
        A.force = true;
    }
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
    pal_desktop_set_chrome(&CHROME);
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
