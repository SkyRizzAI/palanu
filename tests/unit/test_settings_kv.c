/* The settings keys with no owning module (system/src/settings_kv.c) and the table that checks them (settings_api.c): the
 * defaults, the checks (range, names, the text rules), the effects (the key repeat, do not disturb), and the saver's lines
 * and their load back. Uses psys_settings_set, the way the Settings app does. */
#include <string.h>

#include "nema/nema.h"
#include "pal/input.h"
#include "pal/notify.h"
#include "pal/settings.h"
#include "pal/toast.h"
#include "test.h"

static bool has_line(const char *text, const char *line) { return strstr(text, line) != NULL; }

TEST(defaults_and_names) {
    CHECK(psys_settings_kv_has("ui.statusbar"));
    CHECK(psys_settings_kv_has("power.profile"));
    CHECK(!psys_settings_kv_has("no.such.key"));
    CHECK_EQ(psys_settings_int("ui.statusbar", 0), 1);
    CHECK_EQ(psys_settings_int("notify.dnd", 9), 0);
    CHECK_EQ(psys_settings_int("dev.mode", 0), 1); /* on by default, as trust.c has it */
    CHECK(strcmp(psys_settings_text("power.profile"), "balanced") == 0);
    CHECK(strcmp(psys_settings_text("boot.target"), "desktop") == 0);
    CHECK(strcmp(psys_settings_text("no.such.key"), "") == 0);
    CHECK_EQ(psys_settings_int("no.such.key", 7), 7);
}

TEST(set_checks_range_names_and_text) {
    CHECK_EQ(psys_settings_set("notify.dnd", "2"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_settings_set("notify.dnd", "yes"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_settings_set("notify.dnd", "1"), NEMA_OK);
    CHECK_EQ(psys_settings_int("notify.dnd", 0), 1);
    CHECK_EQ(psys_settings_set("notify.dnd", "0"), NEMA_OK);
    CHECK_EQ(psys_settings_set("input.repeat_delay", "50"), NEMA_ERR_INVAL); /* below 120 */
    CHECK_EQ(psys_settings_set("input.repeat_delay", "1500"), NEMA_ERR_INVAL); /* above 1000 */
    CHECK_EQ(psys_settings_set("input.repeat_delay", "500"), NEMA_OK);
    CHECK_EQ(psys_settings_set("power.profile", "7"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_settings_set("power.profile", "turbo"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_settings_set("power.profile", "saver"), NEMA_OK);
    CHECK(strcmp(psys_settings_text("power.profile"), "saver") == 0);
    CHECK_EQ(psys_settings_set("power.profile", "1"), NEMA_OK); /* an index is accepted too */
    CHECK(strcmp(psys_settings_text("power.profile"), "balanced") == 0);
    CHECK_EQ(psys_settings_set("profile.user", "ada lovelace"), NEMA_OK);
    CHECK(strcmp(psys_settings_text("profile.user"), "ada lovelace") == 0);
    CHECK_EQ(psys_settings_set("profile.user", "bad=value"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_settings_set("profile.user", "this name is much longer than twenty three"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_settings_set("boot.target", "console"), NEMA_OK);
    CHECK_EQ(psys_settings_set("boot.target", "terminal"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_settings_set("boot.target", "desktop"), NEMA_OK);
}

TEST(a_set_is_read_back_through_the_table) {
    CHECK_EQ(psys_settings_set("log.level", "debug"), NEMA_OK);
    char out[24];
    CHECK(psys_settings_get("log.level", out, sizeof(out)) > 0);
    CHECK(strcmp(out, "debug") == 0);
    CHECK_EQ(psys_settings_set("log.level", "info"), NEMA_OK);
}

TEST(key_repeat_setting_reaches_the_input_module) {
    CHECK_EQ(psys_settings_set("input.repeat_delay", "600"), NEMA_OK);
    CHECK_EQ(psys_settings_set("input.repeat_rate", "70"), NEMA_OK);
    uint32_t delay = 0, rate = 0;
    pal_input_get_repeat(&delay, &rate);
    CHECK_EQ(delay, 600);
    CHECK_EQ(rate, 70);
    CHECK_EQ(pal_input_long_press_ms(), 550);
    pal_input_set_repeat(10, 10); /* out of range: ignored */
    pal_input_get_repeat(&delay, &rate);
    CHECK_EQ(delay, 600);
    CHECK_EQ(rate, 70);
    CHECK_EQ(psys_settings_set("input.repeat_delay", "420"), NEMA_OK);
    CHECK_EQ(psys_settings_set("input.repeat_rate", "55"), NEMA_OK);
}

TEST(do_not_disturb_drops_toasts) {
    psys_toast_t drop;
    while (psys_toast_take(&drop)) { /* the queue may hold toasts from the tests before */
    }
    CHECK_EQ(psys_settings_set("notify.dnd", "1"), NEMA_OK);
    CHECK_EQ(psys_toast_post("system", "silenced", 2000), 0);
    CHECK_EQ(psys_settings_set("notify.dnd", "0"), NEMA_OK);
    CHECK_EQ(psys_toast_post("system", "audible", 2000), NEMA_OK);
    psys_toast_t t;
    int taken = 0;
    while (psys_toast_take(&t)) {
        CHECK(strcmp(t.text, "silenced") != 0);
        if (strcmp(t.text, "audible") == 0) taken++;
    }
    CHECK_EQ(taken, 1);
}

TEST(the_saver_writes_every_key_and_the_loader_reads_it_back) {
    char text[1024];
    size_t n = psys_settings_kv_format(text, sizeof(text));
    CHECK(n > 100);
    CHECK(has_line(text, "ui.statusbar=1\n"));
    CHECK(has_line(text, "power.profile=balanced\n"));
    CHECK(has_line(text, "log.level=info\n"));
    CHECK(has_line(text, "profile.user=ada lovelace\n"));
    CHECK(has_line(text, "dev.mode=1\n"));
    CHECK(has_line(text, "boot.target=desktop\n"));
    /* a small buffer is refused, not truncated */
    char small[40];
    CHECK_EQ(psys_settings_kv_format(small, sizeof(small)), 0);
    /* loading a line changes the value; a bad line keeps the value */
    psys_settings_kv_load("power.profile", "performance");
    CHECK(strcmp(psys_settings_text("power.profile"), "performance") == 0);
    psys_settings_kv_load("power.profile", "bogus");
    CHECK(strcmp(psys_settings_text("power.profile"), "performance") == 0);
    psys_settings_kv_load("notify.dnd", "7");
    CHECK_EQ(psys_settings_int("notify.dnd", 9), 0);
    psys_settings_kv_load("power.profile", "balanced");
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(defaults_and_names);
    RUN(set_checks_range_names_and_text);
    RUN(a_set_is_read_back_through_the_table);
    RUN(key_repeat_setting_reaches_the_input_module);
    RUN(do_not_disturb_drops_toasts);
    RUN(the_saver_writes_every_key_and_the_loader_reads_it_back);
    return TEST_DONE();
}
