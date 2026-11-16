// PalanuOS launcher test: the home screen has a clock, a memory card and a grid of 8 apps.
// Keyboard navigation: W/A/S/D move, ENTER opens, ESC goes back.
// The apps that really run: Terminal, Notes, Settings, About.
// The rest show a "coming soon" screen.

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_idf_version.h"
#include "board.h"
#include "ui.h"

// ---------------------------------------------------------------- colours and style
#define COL_BG      lv_color_hex(0x0B1120)
#define COL_TEXT    lv_color_hex(0xF1F5F9)
#define COL_MUTED   lv_color_hex(0x94A3B8)
#define COL_ACCENT  lv_color_hex(0x818CF8)

static lv_display_t *disp;
static lv_indev_t *kb;

static lv_style_t st_tile, st_tile_focus, st_row, st_row_focus, st_card;
static lv_style_transition_dsc_t tile_tr;

static lv_obj_t *home_scr;
static lv_group_t *home_grp;
static lv_obj_t *home_time, *home_clock, *home_mem;
static lv_obj_t *cur_time;       // clock label in the status bar of the active screen
static lv_obj_t *pending_time;   // clock label of the app screen being built
static lv_timer_t *app_timer;    // timer owned by the active app (deleted on exit)

static bool anim_on = true;
static bool fps_on = false;
static int brightness = 80;

#define NAPPS 8
#define GRID_COLS 4
static lv_obj_t *tiles[NAPPS];

typedef struct {
    const char *name;
    const char *icon;
    uint32_t color;
    void (*open)(int idx);
} app_t;

static void open_terminal(int idx);
static void open_notes(int idx);
static void open_settings(int idx);
static void open_about(int idx);
static void open_soon(int idx);

static const app_t APPS[NAPPS] = {
    {"Terminal",  LV_SYMBOL_KEYBOARD,  0x10B981, open_terminal},
    {"Notes",   LV_SYMBOL_EDIT,      0xF59E0B, open_notes},
    {"Files",    LV_SYMBOL_DIRECTORY, 0xF97316, open_soon},
    {"Music",     LV_SYMBOL_AUDIO,     0xEC4899, open_soon},
    {"Wi-Fi",     LV_SYMBOL_WIFI,      0x0EA5E9, open_soon},
    {"Bluetooth", LV_SYMBOL_BLUETOOTH, 0x3B82F6, open_soon},
    {"Settings",   LV_SYMBOL_SETTINGS,  0x6366F1, open_settings},
    {"About",   LV_SYMBOL_LIST,      0x64748B, open_about},
};

static void styles_init(void)
{
    static const lv_style_prop_t props[] = {LV_STYLE_TRANSLATE_Y, LV_STYLE_BG_OPA, LV_STYLE_BORDER_OPA, 0};
    lv_style_transition_dsc_init(&tile_tr, props, lv_anim_path_ease_out, 160, 0, NULL);

    lv_style_init(&st_tile);
    lv_style_set_radius(&st_tile, 16);
    lv_style_set_bg_color(&st_tile, lv_color_white());
    lv_style_set_bg_opa(&st_tile, LV_OPA_TRANSP);
    lv_style_set_border_color(&st_tile, lv_color_white());
    lv_style_set_border_width(&st_tile, 2);
    lv_style_set_border_opa(&st_tile, LV_OPA_TRANSP);
    lv_style_set_pad_all(&st_tile, 4);
    lv_style_set_pad_row(&st_tile, 3);
    lv_style_set_transition(&st_tile, &tile_tr);

    lv_style_init(&st_tile_focus);
    lv_style_set_bg_opa(&st_tile_focus, LV_OPA_20);
    lv_style_set_border_opa(&st_tile_focus, LV_OPA_60);
    lv_style_set_translate_y(&st_tile_focus, -4);

    lv_style_init(&st_row);
    lv_style_set_radius(&st_row, 10);
    lv_style_set_bg_color(&st_row, lv_color_hex(0x1E293B));
    lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
    lv_style_set_border_side(&st_row, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&st_row, COL_ACCENT);
    lv_style_set_border_width(&st_row, 0);
    lv_style_set_pad_hor(&st_row, 10);
    lv_style_set_pad_column(&st_row, 8);

    lv_style_init(&st_row_focus);
    lv_style_set_bg_color(&st_row_focus, lv_color_hex(0x334155));
    lv_style_set_border_width(&st_row_focus, 4);

    lv_style_init(&st_card);
    lv_style_set_radius(&st_card, 12);
    lv_style_set_bg_color(&st_card, lv_color_white());
    lv_style_set_bg_opa(&st_card, LV_OPA_10);
    lv_style_set_border_color(&st_card, lv_color_white());
    lv_style_set_border_opa(&st_card, LV_OPA_20);
    lv_style_set_border_width(&st_card, 1);
    lv_style_set_pad_all(&st_card, 8);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

// ---------------------------------------------------------------- clock (simulated, starts at 09:41)
static uint32_t now_secs(void)
{
    return 9 * 3600 + 41 * 60 + (uint32_t)(esp_timer_get_time() / 1000000);
}

static void tick_cb(lv_timer_t *t)
{
    uint32_t s = now_secs();
    char buf[48];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(s / 3600 % 24), (unsigned long)(s / 60 % 60));
    if (cur_time) lv_label_set_text(cur_time, buf);
    lv_label_set_text(home_clock, buf);

    snprintf(buf, sizeof(buf), "%u KB\n%.1f MB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
    lv_label_set_text(home_mem, buf);
}

// ---------------------------------------------------------------- status bar
static lv_obj_t *status_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = plain(scr);
    lv_obj_set_size(bar, LV_PCT(100), 20);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);

    lv_obj_t *brand = text(bar, "PALANU", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_flex_grow(brand, 1);
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    text(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_12, lv_color_hex(0x475569));
    text(bar, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_12, lv_color_hex(0x475569));
    text(bar, LV_SYMBOL_BATTERY_FULL, &lv_font_montserrat_12, lv_color_hex(0x4ADE80));
    return text(bar, "--:--", &lv_font_montserrat_12, COL_TEXT);
}

// ---------------------------------------------------------------- screen navigation
static void go_home(void)
{
    lv_indev_set_group(kb, home_grp);
    cur_time = home_time;
    lv_screen_load_anim(home_scr, anim_on ? LV_SCREEN_LOAD_ANIM_MOVE_RIGHT : LV_SCREEN_LOAD_ANIM_NONE,
                        anim_on ? 220 : 0, 0, true);
}

static void app_screen_deleted(lv_event_t *e)
{
    lv_group_t *g = lv_event_get_user_data(e);
    lv_group_delete(g);
    if (app_timer) {
        lv_timer_delete(app_timer);
        app_timer = NULL;
    }
}

// App screen frame: status bar, coloured header, content area
static lv_obj_t *app_begin(int idx, lv_group_t **grp, lv_obj_t **content)
{
    const app_t *a = &APPS[idx];
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    pending_time = status_bar(scr);

    lv_obj_t *hdr = plain(scr);
    lv_obj_set_size(hdr, LV_PCT(100), 34);
    lv_obj_set_y(hdr, 20);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(a->color), 0);
    lv_obj_set_style_bg_grad_color(hdr, lv_color_darken(lv_color_hex(a->color), 80), 0);
    lv_obj_set_style_bg_grad_dir(hdr, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hdr, 10, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    text(hdr, a->icon, &lv_font_montserrat_20, lv_color_white());
    lv_obj_t *title = text(hdr, a->name, &lv_font_montserrat_20, lv_color_white());
    lv_obj_set_flex_grow(title, 1);
    text(hdr, "ESC " LV_SYMBOL_LEFT, &lv_font_montserrat_12, lv_color_white());

    lv_obj_t *c = plain(scr);
    lv_obj_set_size(c, LV_PCT(100), 240 - 54);
    lv_obj_set_y(c, 54);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ACTIVE);

    *grp = lv_group_create();
    lv_obj_add_event_cb(scr, app_screen_deleted, LV_EVENT_DELETE, *grp);
    *content = c;
    return scr;
}

static void app_show(lv_obj_t *scr, lv_group_t *grp)
{
    lv_indev_set_group(kb, grp);
    cur_time = pending_time;
    lv_screen_load_anim(scr, anim_on ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_NONE,
                        anim_on ? 220 : 0, 0, false);
}

// ESC goes back, W/S move the focus in the list
static void app_nav_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    lv_group_t *g = lv_obj_get_group(lv_event_get_target_obj(e));
    if (k == LV_KEY_ESC) go_home();
    else if (k == 'w' || k == 'W' || k == LV_KEY_UP) lv_group_focus_prev(g);
    else if (k == 's' || k == 'S' || k == LV_KEY_DOWN) lv_group_focus_next(g);
}

static void esc_only_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) go_home();
}

// ---------------------------------------------------------------- Terminal
static lv_obj_t *term_log, *term_scroll;

static void term_print(const char *s)
{
    const char *old = lv_label_get_text(term_log);
    size_t len = strlen(old) + strlen(s) + 2;
    char *buf = lv_malloc(len);
    // keep at most the last ~1500 characters
    const char *start = old;
    if (strlen(old) > 1500) start = strchr(old + strlen(old) - 1500, '\n') ?: old;
    snprintf(buf, len, "%s%s\n", start, s);
    lv_label_set_text(term_log, buf);
    lv_free(buf);
    lv_obj_scroll_to_y(term_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void term_run(const char *cmd)
{
    char line[96];
    snprintf(line, sizeof(line), "$ %s", cmd);
    term_print(line);
    if (!strcmp(cmd, "help")) {
        term_print("commands: help, mem, info, uptime, beep, clear");
    } else if (!strcmp(cmd, "mem")) {
        snprintf(line, sizeof(line), "free internal: %u KB\nfree psram: %u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        term_print(line);
    } else if (!strcmp(cmd, "info")) {
        term_print("PalanuOS (UI test) on ESP32-S31 hhv2");
    } else if (!strcmp(cmd, "uptime")) {
        snprintf(line, sizeof(line), "%llu seconds", esp_timer_get_time() / 1000000);
        term_print(line);
    } else if (!strcmp(cmd, "beep")) {
        board_click();
        term_print("beep!");
    } else if (!strcmp(cmd, "clear")) {
        lv_label_set_text(term_log, "");
    } else if (cmd[0]) {
        snprintf(line, sizeof(line), "unknown: %s (try help)", cmd);
        term_print(line);
    }
}

static void term_ready_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    char cmd[64];
    strncpy(cmd, lv_textarea_get_text(ta), sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = 0;
    for (char *p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);
    lv_textarea_set_text(ta, "");
    term_run(cmd);
}

static void open_terminal(int idx)
{
    lv_group_t *g;
    lv_obj_t *c;
    lv_obj_t *scr = app_begin(idx, &g, &c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x020617), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);

    term_scroll = plain(c);
    lv_obj_set_width(term_scroll, LV_PCT(100));
    lv_obj_set_flex_grow(term_scroll, 1);
    lv_obj_add_flag(term_scroll, LV_OBJ_FLAG_SCROLLABLE);
    term_log = text(term_scroll, "", &lv_font_montserrat_14, lv_color_hex(0x86EFAC));
    lv_obj_set_width(term_log, LV_PCT(100));
    lv_label_set_long_mode(term_log, LV_LABEL_LONG_MODE_WRAP);
    term_print("PalanuOS shell. Type 'help' then ENTER.");

    lv_obj_t *ta = lv_textarea_create(c);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "type a command...");
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(ta, term_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, esc_only_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, ta);
    lv_group_focus_obj(ta);
    app_show(scr, g);
}

// ---------------------------------------------------------------- Notes
static void open_notes(int idx)
{
    lv_group_t *g;
    lv_obj_t *c;
    lv_obj_t *scr = app_begin(idx, &g, &c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ta = lv_textarea_create(c);
    lv_obj_set_size(ta, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_placeholder_text(ta, "Write notes here...\nENTER new line, DEL delete, ESC back.");
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(ta, esc_only_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, ta);
    lv_group_focus_obj(ta);
    app_show(scr, g);
}

// ---------------------------------------------------------------- Settings
enum { SET_ANIM, SET_FPS, SET_CLICK };

static lv_obj_t *row_create(lv_obj_t *parent, lv_group_t *g, const char *icon, const char *label)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_focus, LV_STATE_FOCUSED);
    lv_obj_set_size(r, LV_PCT(100), 38);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(r, icon, &lv_font_montserrat_14, COL_ACCENT);
    lv_obj_t *l = text(r, label, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_group_add_obj(g, r);
    lv_obj_add_event_cb(r, app_nav_cb, LV_EVENT_KEY, NULL);
    return r;
}

static void brightness_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    lv_obj_t *slider = lv_event_get_user_data(e);
    int step = 0;
    if (k == 'a' || k == 'A' || k == LV_KEY_LEFT) step = -10;
    if (k == 'd' || k == 'D' || k == LV_KEY_RIGHT) step = 10;
    if (!step) return;
    brightness += step;
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    lv_slider_set_value(slider, brightness, anim_on ? LV_ANIM_ON : LV_ANIM_OFF);
    board_set_brightness(brightness);
}

static void switch_row_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    lv_obj_t *sw = lv_obj_get_child(row, -1);
    bool on = !lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case SET_ANIM: anim_on = on; break;
    case SET_FPS:
        fps_on = on;
#if LV_USE_PERF_MONITOR
        if (on) lv_sysmon_show_performance(disp);
        else lv_sysmon_hide_performance(disp);
#endif
        break;
    case SET_CLICK: board_set_click_enabled(on); break;
    }
}

static void switch_row(lv_obj_t *parent, lv_group_t *g, const char *icon, const char *label, int id, bool on)
{
    lv_obj_t *r = row_create(parent, g, icon, label);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 40, 20);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(r, switch_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
}

static void open_settings(int idx)
{
    lv_group_t *g;
    lv_obj_t *c;
    lv_obj_t *scr = app_begin(idx, &g, &c);

    lv_obj_t *r = row_create(c, g, LV_SYMBOL_IMAGE, "Brightness");
    lv_obj_t *sl = lv_slider_create(r);
    lv_obj_set_width(sl, 110);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, brightness, LV_ANIM_OFF);
    lv_obj_remove_flag(sl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, brightness_key_cb, LV_EVENT_KEY, sl);

    switch_row(c, g, LV_SYMBOL_SHUFFLE, "Transition animation", SET_ANIM, anim_on);
    switch_row(c, g, LV_SYMBOL_EYE_OPEN, "Show FPS", SET_FPS, fps_on);
    switch_row(c, g, LV_SYMBOL_VOLUME_MAX, "Key sound", SET_CLICK, board_click_enabled());

    text(c, "W/S select   A/D change   ENTER toggle", &lv_font_montserrat_12, COL_MUTED);
    app_show(scr, g);
}

// ---------------------------------------------------------------- About
static lv_obj_t *about_dyn;

static void about_tick(lv_timer_t *t)
{
    uint64_t s = esp_timer_get_time() / 1000000;
    lv_label_set_text_fmt(about_dyn,
                          "Free internal RAM: %u KB\nFree PSRAM: %u KB\nUptime: %02u:%02u:%02u",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

static void open_about(int idx)
{
    lv_group_t *g;
    lv_obj_t *c;
    lv_obj_t *scr = app_begin(idx, &g, &c);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    char buf[200];
    snprintf(buf, sizeof(buf),
             "Chip: ESP32-S31 rev v%d.%d, %d core\nPSRAM: %u MB (octal 200 MHz)\nLCD: ILI9341 320x240, SPI 80 MHz\nUI: LVGL %d.%d.%d\nESP-IDF: %s",
             ci.revision / 100, ci.revision % 100, ci.cores,
             (unsigned)(esp_psram_is_initialized() ? esp_psram_get_size() / 1048576 : 0),
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, esp_get_idf_version());

    lv_obj_t *card = lv_obj_create(c);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_t *info = text(card, buf, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_style_text_line_space(info, 3, 0);

    lv_obj_t *card2 = lv_obj_create(c);
    lv_obj_remove_style_all(card2);
    lv_obj_add_style(card2, &st_card, 0);
    lv_obj_set_width(card2, LV_PCT(100));
    lv_obj_set_height(card2, LV_SIZE_CONTENT);
    about_dyn = text(card2, "", &lv_font_montserrat_14, lv_color_hex(0x86EFAC));
    lv_obj_set_style_text_line_space(about_dyn, 3, 0);
    about_tick(NULL);
    app_timer = lv_timer_create(about_tick, 1000, NULL);

    lv_group_add_obj(g, c);
    lv_obj_add_event_cb(c, app_nav_cb, LV_EVENT_KEY, NULL);
    app_show(scr, g);
}

// ---------------------------------------------------------------- Coming soon
static void open_soon(int idx)
{
    lv_group_t *g;
    lv_obj_t *c;
    lv_obj_t *scr = app_begin(idx, &g, &c);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = plain(c);
    lv_obj_set_size(icon, 64, 64);
    lv_obj_set_style_radius(icon, 20, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(APPS[idx].color), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_t *sym = text(icon, APPS[idx].icon, &lv_font_montserrat_28, lv_color_white());
    lv_obj_center(sym);

    text(c, "Coming soon", &lv_font_montserrat_20, COL_TEXT);
    text(c, "This app is not built yet. ESC to go back.", &lv_font_montserrat_12, COL_MUTED);

    lv_group_add_obj(g, c);
    lv_obj_add_event_cb(c, app_nav_cb, LV_EVENT_KEY, NULL);
    app_show(scr, g);
}

// ---------------------------------------------------------------- home screen
static void tile_key_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t k = lv_event_get_key(e);
    int n = i;
    if (k == 'a' || k == 'A' || k == LV_KEY_LEFT) n = i - 1;
    else if (k == 'd' || k == 'D' || k == LV_KEY_RIGHT) n = i + 1;
    else if (k == 'w' || k == 'W' || k == LV_KEY_UP) n = i - GRID_COLS;
    else if (k == 's' || k == 'S' || k == LV_KEY_DOWN) n = i + GRID_COLS;
    if (n != i && n >= 0 && n < NAPPS) lv_group_focus_obj(tiles[n]);
}

static void tile_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    APPS[i].open(i);
}

static void blob(lv_obj_t *parent, int x, int y, int size, uint32_t color)
{
    lv_obj_t *b = plain(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
}

static void build_home(void)
{
    home_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(home_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(home_scr, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_grad_color(home_scr, lv_color_hex(0x312E81), 0);
    lv_obj_set_style_bg_grad_dir(home_scr, LV_GRAD_DIR_VER, 0);

    // wallpaper: faint coloured circles
    blob(home_scr, 210, -40, 160, 0x7C3AED);
    blob(home_scr, -60, 140, 150, 0x0891B2);
    blob(home_scr, 120, 170, 90, 0xDB2777);

    home_time = status_bar(home_scr);

    // big clock and date
    home_clock = text(home_scr, "09:41", &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_pos(home_clock, 14, 26);
    lv_obj_t *date = text(home_scr, "Thursday, 12 November", &lv_font_montserrat_14, lv_color_hex(0xC7D2FE));
    lv_obj_set_pos(date, 15, 58);

    // memory card
    lv_obj_t *card = plain(home_scr);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_set_size(card, 132, 48);
    lv_obj_set_pos(card, 178, 28);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 6, 0);
    lv_obj_t *keys = text(card, "RAM\nPSRAM", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_style_text_line_space(keys, 2, 0);
    home_mem = text(card, "", &lv_font_montserrat_12, COL_TEXT);
    lv_obj_set_style_text_line_space(home_mem, 2, 0);

    // 4x2 app grid
    lv_obj_t *grid = plain(home_scr);
    lv_obj_set_size(grid, 320, 140);
    lv_obj_set_pos(grid, 0, 80);
    lv_obj_set_style_pad_hor(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 2, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    home_grp = lv_group_create();
    for (int i = 0; i < NAPPS; i++) {
        lv_obj_t *t = lv_obj_create(grid);
        lv_obj_remove_style_all(t);
        lv_obj_add_style(t, &st_tile, 0);
        lv_obj_add_style(t, &st_tile_focus, LV_STATE_FOCUSED);
        lv_obj_set_size(t, 72, 68);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *ic = plain(t);
        lv_obj_set_size(ic, 42, 42);
        lv_obj_set_style_radius(ic, 13, 0);
        lv_obj_set_style_bg_color(ic, lv_color_hex(APPS[i].color), 0);
        lv_obj_set_style_bg_grad_color(ic, lv_color_darken(lv_color_hex(APPS[i].color), 70), 0);
        lv_obj_set_style_bg_grad_dir(ic, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(ic, 12, 0);
        lv_obj_set_style_shadow_color(ic, lv_color_hex(APPS[i].color), 0);
        lv_obj_set_style_shadow_opa(ic, LV_OPA_40, 0);
        lv_obj_t *sym = text(ic, APPS[i].icon, &lv_font_montserrat_20, lv_color_white());
        lv_obj_center(sym);

        text(t, APPS[i].name, &lv_font_montserrat_12, COL_TEXT);

        lv_obj_add_event_cb(t, tile_key_cb, LV_EVENT_KEY, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(home_grp, t);
        tiles[i] = t;
    }

    lv_obj_t *hint = text(home_scr, "W A S D  move    ENTER  open    ESC  back", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void ui_init(lv_display_t *d, lv_indev_t *keyboard)
{
    disp = d;
    kb = keyboard;
    lv_theme_t *th = lv_theme_default_init(disp, COL_ACCENT, lv_color_hex(0x22D3EE), true, &lv_font_montserrat_14);
    lv_display_set_theme(disp, th);
#if LV_USE_PERF_MONITOR
    lv_sysmon_hide_performance(disp);
#endif
    styles_init();
    build_home();
    lv_indev_set_group(kb, home_grp);
    lv_group_focus_obj(tiles[0]);
    cur_time = home_time;
    lv_screen_load(home_scr);
    tick_cb(NULL);
    lv_timer_create(tick_cb, 1000, NULL);
}
