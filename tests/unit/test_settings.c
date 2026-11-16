/* The settings table of the palanu settings API through psys_api_setting_get/set, with a native caller. The values
 * the system applies (theme, brightness, time zone) act on the real system modules; the values the built-in apps own
 * (terminal font, radio policy, device name) go through a fake host, bound here as the apps bind theirs at boot. The
 * save hook counts the saves, so the persistence call is checked (the file itself is written by apps/settings_store.c). */
#include <stdio.h>
#include <string.h>

#include "../../system/src/api_impl.h"
#include "nema/nema.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/settings.h"
#include "pal/theme.h"
#include "pal/time.h"
#include "test.h"

static api_ctx_t *NO_CTX = NULL;

/* ---- the fake host: the app-owned values ---- */
static int h_font = 1;
static char h_name[32] = "palanu";
static int h_saves;
static int h_font_sets;
static const char *h_radio_wifi = "auto";
static const char *h_radio_ble = "off";
static int h_name_sets;

static int f_font_get(void) { return h_font; }
static void f_font_set(int f) {
    h_font = f;
    h_font_sets++;
}
static const char *f_radio_get(const char *key) {
    if (strcmp(key, "radio.wifi") == 0) return h_radio_wifi;
    if (strcmp(key, "radio.ble") == 0) return h_radio_ble;
    return NULL;
}
static const char *f_name_get(void) { return h_name; }
static int f_name_set(const char *n) {
    h_name_sets++;
    snprintf(h_name, sizeof(h_name), "%s", n);
    return NEMA_OK;
}
static void f_save(void) { h_saves++; }

static const psys_settings_host_t HOST = {f_font_get, f_font_set, f_radio_get, f_name_get, f_name_set, f_save};

/* ---- the change callback ---- */
static int cb_count;
static char cb_key[32];
static void on_change(const char *key) {
    cb_count++;
    snprintf(cb_key, sizeof(cb_key), "%s", key);
}

static int32_t get(const char *k, char *out, uint32_t cap) { return psys_api_setting_get(NO_CTX, k, out, cap); }
static int32_t set(const char *k, const char *v) { return psys_api_setting_set(NO_CTX, k, v); }

static int eq(const char *k, const char *want) {
    char buf[40];
    int32_t n = get(k, buf, sizeof(buf));
    return n >= 0 && (size_t)n == strlen(want) && strcmp(buf, want) == 0;
}

/* ---- tests ---- */

TEST(ui_dark_reads_and_applies_live) {
    CHECK_EQ(set("ui.dark", "1"), NEMA_OK);
    CHECK(pal_theme_dark());
    CHECK(eq("ui.dark", "1"));
    CHECK_EQ(set("ui.dark", "0"), NEMA_OK);
    CHECK(!pal_theme_dark());
    CHECK(eq("ui.dark", "0"));
    CHECK_EQ(set("ui.dark", "2"), NEMA_ERR_INVAL);
    CHECK_EQ(set("ui.dark", "yes"), NEMA_ERR_INVAL);
    CHECK_EQ(set("ui.dark", ""), NEMA_ERR_INVAL);
    CHECK_EQ(set("ui.dark", "1 "), NEMA_ERR_INVAL);
}

TEST(ui_theme_by_index_or_name) {
    CHECK(pal_theme_count() >= 2);
    const char *second = pal_theme_name(1);
    CHECK_EQ(set("ui.theme", second), NEMA_OK);
    CHECK_EQ(pal_theme_index(), 1);
    CHECK(eq("ui.theme", second));
    CHECK_EQ(set("ui.theme", "0"), NEMA_OK);
    CHECK_EQ(pal_theme_index(), 0);
    CHECK_EQ(set("ui.theme", "99"), NEMA_ERR_INVAL);
    CHECK_EQ(set("ui.theme", "no-such-theme"), NEMA_ERR_INVAL);
}

TEST(ui_size_is_zero_or_one) {
    CHECK_EQ(set("ui.size", "1"), NEMA_OK);
    CHECK_EQ(pal_theme_size(), 1);
    CHECK(eq("ui.size", "1"));
    CHECK_EQ(set("ui.size", "2"), NEMA_ERR_INVAL);
    CHECK_EQ(set("ui.size", "0"), NEMA_OK);
}

TEST(brightness_applies_to_the_display_and_is_range_checked) {
    CHECK_EQ(set("display.brightness", "50"), NEMA_OK);
    CHECK_EQ(pal_display_brightness(), 50);
    CHECK(eq("display.brightness", "50"));
    CHECK_EQ(set("display.brightness", "101"), NEMA_ERR_INVAL);
    CHECK_EQ(set("display.brightness", "-1"), NEMA_ERR_INVAL);
    CHECK_EQ(set("display.brightness", "7x"), NEMA_ERR_INVAL);
    CHECK_EQ(set("display.brightness", "1234567"), NEMA_ERR_INVAL);
    CHECK_EQ(pal_display_brightness(), 50);
}

TEST(time_zone_in_minutes) {
    CHECK_EQ(set("time.tz", "-300"), NEMA_OK);
    CHECK_EQ(pal_time_tz_minutes(), -300);
    CHECK(eq("time.tz", "-300"));
    CHECK_EQ(set("time.tz", "841"), NEMA_ERR_INVAL);
    CHECK_EQ(set("time.tz", "-721"), NEMA_ERR_INVAL);
    CHECK_EQ(set("time.tz", "0"), NEMA_OK);
}

TEST(app_owned_values_need_the_host_and_are_saved_through_it) {
    psys_settings_bind(&HOST);
    CHECK(eq("term.font", "1"));
    CHECK_EQ(set("term.font", "2"), NEMA_OK);
    CHECK_EQ(h_font, 2);
    CHECK(eq("term.font", "2"));
    CHECK_EQ(set("term.font", "3"), NEMA_ERR_INVAL);
    CHECK_EQ(h_font_sets, 1);

    CHECK(eq("radio.wifi", "auto"));
    CHECK(eq("radio.ble", "off"));
    CHECK_EQ(set("radio.ble", "on"), NEMA_ERR_PERM); /* read only: the wifi and ble commands change it */
    CHECK_EQ(set("radio.wifi", "on"), NEMA_ERR_PERM);

    CHECK(eq("device.name", "palanu"));
    CHECK_EQ(set("device.name", "hhv2-desk"), NEMA_OK);
    CHECK(eq("device.name", "hhv2-desk"));
    CHECK_EQ(set("device.name", "bad name!"), NEMA_ERR_INVAL);
    CHECK_EQ(set("device.name", ""), NEMA_ERR_INVAL);
    CHECK_EQ(set("device.name", "0123456789012345678901234567890123"), NEMA_ERR_INVAL); /* 34 */
    CHECK_EQ(set("device.name", "0123456789012345678901234567890"), NEMA_OK); /* 31 is the most */
    CHECK_EQ(h_name_sets, 2);
}

TEST(every_change_is_saved_and_reported_and_a_refusal_is_neither) {
    psys_settings_on_change(on_change);
    int saves = h_saves;
    cb_count = 0;
    CHECK_EQ(set("ui.size", "1"), NEMA_OK);
    CHECK_EQ(h_saves, saves + 1);
    CHECK_EQ(cb_count, 1);
    CHECK(strcmp(cb_key, "ui.size") == 0);
    CHECK_EQ(set("ui.size", "9"), NEMA_ERR_INVAL);
    CHECK_EQ(set("radio.ble", "on"), NEMA_ERR_PERM);
    CHECK_EQ(set("no.such.key", "1"), NEMA_ERR_NOENT);
    CHECK_EQ(h_saves, saves + 1);
    CHECK_EQ(cb_count, 1);
    psys_settings_on_change(NULL);
}

TEST(unknown_keys_and_the_buffer_rules) {
    char buf[8];
    CHECK_EQ(get("no.such.key", buf, sizeof(buf)), NEMA_ERR_NOENT);
    CHECK_EQ(set("no.such.key", "1"), NEMA_ERR_NOENT);
    CHECK_EQ(get("ui.dark", buf, 1), NEMA_ERR_RANGE); /* a one-digit value needs its NUL too */
    CHECK_EQ(get("ui.dark", buf, 2), 1);
    CHECK(strcmp(buf, "0") == 0 || strcmp(buf, "1") == 0);
    CHECK_EQ(get("ui.theme", buf, 2), NEMA_ERR_RANGE);
    CHECK_EQ(psys_api_setting_get(NO_CTX, NULL, buf, sizeof(buf)), NEMA_ERR_INVAL);
}

TEST(app_owned_values_are_not_there_without_the_host) {
    char buf[32];
    psys_settings_bind(NULL);
    CHECK_EQ(get("term.font", buf, sizeof(buf)), NEMA_ERR_NOENT);
    CHECK_EQ(get("radio.wifi", buf, sizeof(buf)), NEMA_ERR_NOENT);
    CHECK_EQ(get("device.name", buf, sizeof(buf)), NEMA_ERR_NOENT);
    CHECK_EQ(set("term.font", "1"), NEMA_ERR_NOENT);
    CHECK_EQ(set("device.name", "x"), NEMA_ERR_NOENT);
    CHECK_EQ(get("ui.dark", buf, sizeof(buf)) >= 0, 1); /* the system's own values still answer */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_theme_select(0, false, 0);
    pal_display_set_brightness(80);
    RUN(ui_dark_reads_and_applies_live);
    RUN(ui_theme_by_index_or_name);
    RUN(ui_size_is_zero_or_one);
    RUN(brightness_applies_to_the_display_and_is_range_checked);
    RUN(time_zone_in_minutes);
    RUN(app_owned_values_need_the_host_and_are_saved_through_it);
    RUN(every_change_is_saved_and_reported_and_a_refusal_is_neither);
    RUN(unknown_keys_and_the_buffer_rules);
    RUN(app_owned_values_are_not_there_without_the_host);
    return TEST_DONE();
}
