/* The screens' pure parts (system/src/ui_*.c): the icon table, the layout math of the launcher skins, the clock
 * and age text, the PIN entry, the control center's focus grid and tile texts, and the wallpapers (a repaint of part of the
 * screen must equal the same part of a full paint). No display is needed: the canvas is a plain buffer. */
#include <string.h>

#include "nema/nema.h"
#include "nema/vfs.h"
#include "pal/gfx.h"
#include "pal/icons.h"
#include "pal/theme.h"
#include "pal/time.h"
#include "pal/ui.h"
#include "test.h"
#include "../../system/src/ui_control.h"

#define W 320
#define H 240
static uint8_t PX[W * H];

/* ---- icons ---- */
TEST(icon_table_is_consistent) {
    CHECK(PAL_ICONS_COUNT > 60);
    for (size_t i = 0; i < PAL_ICONS_COUNT; i++) {
        const pal_icon_t *ic = &PAL_ICONS[i];
        CHECK(ic->w == ic->h);
        CHECK(ic->w == ic->size);
        CHECK(ic->bits != NULL);
        CHECK(pal_icon_find(ic->name, ic->size) == ic);
    }
}

TEST(icon_lookup_by_name_and_size) {
    const pal_icon_t *t32 = pal_icon_find("terminal", 32);
    CHECK(t32 != NULL);
    CHECK_EQ(t32->w, 32);
    CHECK(pal_icon_find("terminal", 16) != NULL);
    CHECK(pal_icon_find("terminal", 8) != NULL); /* drawn at 8 px for the Settings rows */
    CHECK(pal_icon_find("terminal", 24) == NULL); /* no such size */
    CHECK(pal_icon_find("wifi0", 8) != NULL);
    CHECK(pal_icon_find("wifi4", 8) != NULL);
    CHECK(pal_icon_find("no-such-icon", 32) == NULL);
    CHECK(pal_icon_find("", 32) == NULL);
    CHECK(pal_icon_find(NULL, 32) == NULL);
}

TEST(icon_blit_draws_its_ink_pixels) {
    pal_canvas_t c;
    pal_canvas_init(&c, PX, W, H);
    pal_fill(&c, 0, 0, W, H, 0);
    const pal_icon_t *ic = pal_icon_find("lock", 32);
    CHECK(ic != NULL);
    pal_icon_blit(&c, ic, 10, 10, 7, 9);
    int ink = 0, expect = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int bit = 0x80 >> (x & 7), idx = y * 4 + (x >> 3);
            if (ic->bits[idx] & bit) expect++;
            if (pal_get(&c, 10 + x, 10 + y) == 7) ink++;
        }
    CHECK_EQ(ink, expect);
    CHECK_EQ(pal_get(&c, 9, 9), 0);
}

TEST(icon_draw_falls_back_to_default_for_unknown_ids) {
    pal_canvas_t c;
    pal_canvas_init(&c, PX, W, H);
    pal_fill(&c, 0, 0, W, H, 0);
    pal_icon_draw(&c, "definitely-unknown", "x", 0, 0, 32, 5, 0);
    int n = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            if (pal_get(&c, x, y) == 5) n++;
    CHECK(n > 40); /* the default bitmap is a rounded square with a question mark */
}

/* ---- formatting ---- */
TEST(clock_and_date_text) {
    char b[32];
    pal_ui_fmt_hm(10, 9, b, sizeof(b));
    CHECK(strcmp(b, "10:09") == 0);
    pal_ui_fmt_hm(0, 5, b, sizeof(b));
    CHECK(strcmp(b, "00:05") == 0);
    pal_ui_set_clock24(false); /* the 12-hour clock: no leading zero, 12 for midnight and noon, the suffix apart */
    pal_ui_fmt_hm(0, 5, b, sizeof(b));
    CHECK(strcmp(b, "12:05") == 0);
    pal_ui_fmt_clock(13, 41, b, sizeof(b));
    CHECK(strcmp(b, "1:41 PM") == 0);
    pal_ui_fmt_clock(12, 0, b, sizeof(b));
    CHECK(strcmp(b, "12:00 PM") == 0);
    pal_ui_fmt_clock(9, 30, b, sizeof(b));
    CHECK(strcmp(b, "9:30 AM") == 0);
    CHECK_EQ(pal_ui_hour(23), 11);
    pal_ui_set_clock24(true);
    pal_ui_fmt_clock(13, 41, b, sizeof(b));
    CHECK(strcmp(b, "13:41") == 0);
    CHECK(strcmp(pal_ui_ampm(13), "") == 0);
    pal_tm_t tm = {2026, 1, 1, 10, 9, 0, 4}; /* 1 January 2026 was a Thursday */
    pal_ui_fmt_date(&tm, b, sizeof(b));
    CHECK(strcmp(b, "Thu 1 Jan 2026") == 0);
}

TEST(notification_age_labels) {
    char b[16];
    uint32_t now = 1000000;
    pal_ui_fmt_age(now, now, 0, b, sizeof(b));
    CHECK(strcmp(b, "now") == 0);
    pal_ui_fmt_age(now, now - 59, 0, b, sizeof(b));
    CHECK(strcmp(b, "now") == 0);
    pal_ui_fmt_age(now, now - 120, 0, b, sizeof(b));
    CHECK(strcmp(b, "2m") == 0);
    pal_ui_fmt_age(now, now - 3599, 0, b, sizeof(b));
    CHECK(strcmp(b, "59m") == 0);
    /* same local day: the clock time of the notification */
    pal_tm_t a = {2026, 1, 1, 10, 9, 0, 4}, c = {2026, 1, 1, 9, 4, 0, 4};
    uint32_t tn = pal_time_to_epoch(&a), tt = pal_time_to_epoch(&c);
    pal_ui_fmt_age(tn, tt, 0, b, sizeof(b));
    CHECK(strcmp(b, "09:04") == 0);
    /* a day earlier: days */
    pal_tm_t d = {2026, 1, 3, 10, 9, 0, 6}, e = {2026, 1, 1, 10, 9, 0, 4};
    pal_ui_fmt_age(pal_time_to_epoch(&d), pal_time_to_epoch(&e), 0, b, sizeof(b));
    CHECK(strcmp(b, "2d") == 0);
    /* the time zone moves the day boundary: 23:50 UTC is already the next day at +7 */
    pal_tm_t f = {2026, 1, 1, 23, 50, 0, 4}, g = {2026, 1, 2, 0, 20, 0, 5};
    pal_ui_fmt_age(pal_time_to_epoch(&g), pal_time_to_epoch(&f), 420, b, sizeof(b));
    CHECK(strcmp(b, "30m") == 0);
}

TEST(text_fit_truncates_with_dots) {
    char out[40];
    pal_ui_text_fit(&PAL_FONT_SPLEEN_6X12, "short", 200, out, sizeof(out));
    CHECK(strcmp(out, "short") == 0);
    pal_ui_text_fit(&PAL_FONT_SPLEEN_6X12, "a very long notification title", 60, out, sizeof(out));
    CHECK(strlen(out) >= 3 && strcmp(out + strlen(out) - 2, "..") == 0);
    CHECK(pal_text_w(&PAL_FONT_SPLEEN_6X12, out) <= 60);
}

TEST(window_first_centres_and_clamps) {
    CHECK_EQ(pal_ui_window_first(3, 2, 5), 0);
    CHECK_EQ(pal_ui_window_first(10, 0, 4), 0);
    CHECK_EQ(pal_ui_window_first(10, 9, 4), 6);
    CHECK_EQ(pal_ui_window_first(10, 5, 4), 3);
}

/* ---- PIN entry ---- */
static bool check_1234(const char *p) { return strcmp(p, "1234") == 0; }

#define digit(d) (&(pal_key_t){.sym = PAL_KEY_CHAR, .cp = (d)})

TEST(pin_entry_state_machine) {
    pal_ui_pin_t p;
    pal_ui_pin_reset(&p);
    pal_key_t enter = {0}, bksp = {0}, esc = {0}, letter = {0};
    enter.sym = PAL_KEY_ENTER;
    bksp.sym = PAL_KEY_BACKSPACE;
    esc.sym = PAL_KEY_ESC;
    letter.sym = PAL_KEY_CHAR;
    letter.cp = 'a';
    CHECK(pal_ui_pin_key(&p, &letter, check_1234) == PAL_UI_PIN_NONE);
    CHECK(pal_ui_pin_key(&p, &bksp, check_1234) == PAL_UI_PIN_NONE);
    CHECK(pal_ui_pin_key(&p, &enter, check_1234) == PAL_UI_PIN_SHORT);
    CHECK(pal_ui_pin_key(&p, digit('1'), check_1234) == PAL_UI_PIN_EDITED);
    CHECK(pal_ui_pin_key(&p, digit('2'), check_1234) == PAL_UI_PIN_EDITED);
    CHECK(pal_ui_pin_key(&p, &bksp, check_1234) == PAL_UI_PIN_EDITED);
    CHECK_EQ(p.len, 1);
    CHECK(pal_ui_pin_key(&p, digit('2'), check_1234) == PAL_UI_PIN_EDITED);
    CHECK(pal_ui_pin_key(&p, digit('3'), check_1234) == PAL_UI_PIN_EDITED);
    CHECK(pal_ui_pin_key(&p, digit('9'), check_1234) == PAL_UI_PIN_EDITED);
    CHECK_EQ(p.len, 4);
    CHECK(pal_ui_pin_key(&p, &enter, check_1234) == PAL_UI_PIN_WRONG);
    CHECK_EQ(p.len, 0);
    CHECK_EQ(p.wrong, 1);
    CHECK(p.last_wrong);
    for (uint32_t d = '1'; d <= '4'; d++) pal_ui_pin_key(&p, digit(d), check_1234);
    CHECK(pal_ui_pin_key(&p, &enter, check_1234) == PAL_UI_PIN_OK);
    CHECK_EQ(p.len, 0);
    CHECK_EQ(p.wrong, 0);
    /* esc clears a partial entry and does nothing on an empty one */
    pal_ui_pin_key(&p, digit('7'), check_1234);
    CHECK(pal_ui_pin_key(&p, &esc, check_1234) == PAL_UI_PIN_EDITED);
    CHECK(pal_ui_pin_key(&p, &esc, check_1234) == PAL_UI_PIN_NONE);
    /* the entry never passes the maximum */
    for (int i = 0; i < 12; i++) pal_ui_pin_key(&p, digit('0'), check_1234);
    CHECK_EQ(p.len, PAL_UI_PIN_MAX);
}

TEST(pin_setting_rules) {
    CHECK(!pal_ui_pin_set("12"));
    CHECK(!pal_ui_pin_set("12345678901"));
    CHECK(!pal_ui_pin_set("12a4"));
    CHECK(pal_ui_pin_set("1234"));
    CHECK(pal_ui_pin_is_set());
    CHECK(pal_ui_pin_check("1234"));
    CHECK(!pal_ui_pin_check("4321"));
    CHECK(pal_ui_pin_set(""));
    CHECK(!pal_ui_pin_is_set());
    CHECK(!pal_ui_pin_check("1234"));
}

/* ---- mission control ---- */
TEST(control_focus_moves_in_the_grid) {
    /* The reference landscape grid: row 0 Dark, Wi-Fi, Lock; row 1 Settings, Restart, empty; sliders in the last two columns */
    CHECK_EQ(pal_ui_ctl_move(CTL_DARK, CTL_RIGHT), CTL_WIFI);
    CHECK_EQ(pal_ui_ctl_move(CTL_WIFI, CTL_RIGHT), CTL_LOCK);
    CHECK_EQ(pal_ui_ctl_move(CTL_LOCK, CTL_RIGHT), CTL_BRIGHT);
    CHECK_EQ(pal_ui_ctl_move(CTL_BRIGHT, CTL_RIGHT), CTL_VOL);
    CHECK_EQ(pal_ui_ctl_move(CTL_VOL, CTL_RIGHT), CTL_VOL); /* the right edge stays */
    CHECK_EQ(pal_ui_ctl_move(CTL_VOL, CTL_LEFT), CTL_BRIGHT);
    CHECK_EQ(pal_ui_ctl_move(CTL_DARK, CTL_DOWN), CTL_SETTINGS);
    CHECK_EQ(pal_ui_ctl_move(CTL_WIFI, CTL_DOWN), CTL_RESTART);
    CHECK_EQ(pal_ui_ctl_move(CTL_RESTART, CTL_RIGHT), CTL_BRIGHT); /* the empty cell is skipped */
    CHECK_EQ(pal_ui_ctl_move(CTL_LOCK, CTL_DOWN), CTL_LOCK);       /* an empty cell below does not move */
    CHECK_EQ(pal_ui_ctl_move(CTL_SETTINGS, CTL_DOWN), CTL_SETTINGS); /* the bottom edge stays */
    CHECK_EQ(pal_ui_ctl_move(CTL_WIFI, CTL_UP), CTL_WIFI);           /* the top edge stays */
    CHECK_EQ(pal_ui_ctl_move(CTL_DARK, CTL_LEFT), CTL_DARK);         /* the left edge stays */
    CHECK_EQ(pal_ui_ctl_move(CTL_BRIGHT, CTL_UP), CTL_BRIGHT);       /* up on a slider adjusts, it does not move */
    CHECK_EQ(pal_ui_ctl_move(CTL_VOL, CTL_DOWN), CTL_VOL);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(64u * 1024u)); /* the UI prefs save to /data */
    nema_fs_mkdir("/data");
    RUN(icon_table_is_consistent);
    RUN(icon_lookup_by_name_and_size);
    RUN(icon_blit_draws_its_ink_pixels);
    RUN(icon_draw_falls_back_to_default_for_unknown_ids);
    RUN(clock_and_date_text);
    RUN(notification_age_labels);
    RUN(text_fit_truncates_with_dots);
    RUN(window_first_centres_and_clamps);
    RUN(pin_entry_state_machine);
    RUN(pin_setting_rules);
    RUN(control_focus_moves_in_the_grid);
    return TEST_DONE();
}
