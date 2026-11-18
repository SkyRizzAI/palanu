/* The desktop registry (system/src/desktops.c) and the system console under it (system/src/console.c, ADR-0010): registration
 * rules, switching with stop and start, the choice kept in `desktop.env`, and no fallback desktop: a session that fails, an
 * unknown name, `desktop stop` and boot.target console all leave no session and the console in front. No desktop task runs in
 * this process, so the console's calls run directly. */
#include <string.h>

#include "nema/nema.h"
#include "pal/console.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/display.h"
#include "pal/settings.h"
#include "test.h"

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) { (void)ctx; (void)px; (void)stride; (void)r; (void)pal; }

static int starts_a, stops_a, starts_b, starts_bad;

static bool start_classic(void) { return true; }
static bool start_a(void) { starts_a++; pal_desktop_set_home("a-home"); return true; }
static void stop_a(void) { stops_a++; }
static bool start_b(void) { starts_b++; pal_desktop_set_home("b-home"); return true; }
static bool start_bad(void) { starts_bad++; return false; }

static const pal_desktop_env_t A = {.id = "a", .title = "A", .about = "first", .start = start_a, .stop = stop_a};
static const pal_desktop_env_t B = {.id = "b", .title = "B", .about = "second", .start = start_b};
static const pal_desktop_env_t BAD = {.id = "bad", .title = "Bad", .about = "never starts", .start = start_bad};
static const pal_desktop_env_t CLASSIC = {.id = "classic", .title = "Classic", .about = "no longer special", .start = start_classic};

TEST(registration_rejects_bad_and_duplicate_entries) {
    pal_desktop_env_t none = {.id = "x"}; /* no start */
    pal_desktop_env_t noid = {.start = start_a};
    CHECK(pal_desktops_register(NULL) == NEMA_ERR_INVAL);
    CHECK(pal_desktops_register(&none) == NEMA_ERR_INVAL);
    CHECK(pal_desktops_register(&noid) == NEMA_ERR_INVAL);
    CHECK(pal_desktops_register(&A) == NEMA_OK);
    CHECK(pal_desktops_register(&A) == NEMA_ERR_EXIST);
    CHECK(pal_desktops_register(&B) == NEMA_OK);
    CHECK(pal_desktops_register(&BAD) == NEMA_OK);
    CHECK(pal_desktops_register(&CLASSIC) == NEMA_OK);
    CHECK_EQ((long)pal_desktops_count(), 4);
    CHECK(pal_desktops_find("b") == &B);
    CHECK(pal_desktops_find("nope") == NULL);
    CHECK(pal_desktops_find(NULL) == NULL);
    CHECK(pal_desktops_get(99) == NULL);
}

TEST(use_switches_stops_the_old_one_and_keeps_the_choice) {
    CHECK(pal_desktops_use("nope") == NEMA_ERR_NOENT);
    CHECK(pal_desktops_use("a") == NEMA_OK);
    CHECK_EQ(starts_a, 1);
    CHECK(strcmp(pal_desktops_active(), "a") == 0);
    CHECK(strcmp(pal_desktop_home_id(), "a-home") == 0);
    char buf[24] = "";
    psys_settings_get("desktop.env", buf, sizeof(buf));
    CHECK(strcmp(buf, "a") == 0);
    CHECK(pal_desktops_use("b") == NEMA_OK);
    CHECK_EQ(stops_a, 1); /* the old desktop was stopped, once */
    CHECK_EQ(starts_b, 1);
    CHECK(strcmp(pal_desktop_home_id(), "b-home") == 0);
}

static pal_key_t switch_key(void) {
    pal_key_t k = {PAL_KEY_ESC, PAL_MOD_CTRL | PAL_MOD_ALT, false, 0};
    return k;
}

TEST(the_console_is_in_front_until_the_boot_hands_the_screen_to_the_session) {
    CHECK(pal_console_front()); /* from pal_console_init */
    pal_key_t k = switch_key();
    CHECK(pal_console_key(&k)); /* taken, but the boot is not over: nothing changes */
    CHECK(pal_console_front());
    pal_console_pump(1);
    pal_console_pump(6000); /* past the longest boot wait */
    CHECK(!pal_console_front()); /* session "b" is running: it has the screen */
    uint32_t gen = pal_console_generation();
    CHECK(pal_console_key(&k));
    CHECK(pal_console_front());
    CHECK(pal_console_generation() == gen + 1); /* the session will draw everything again */
    CHECK(pal_console_key(&k));
    CHECK(!pal_console_front());
    pal_key_t a = {PAL_KEY_CHAR, 0, false, 'x'};
    CHECK(!pal_console_key(&a)); /* other keys go to the session */
}

TEST(a_desktop_that_fails_leaves_no_session_and_the_console_in_front) {
    CHECK(pal_desktops_use("bad") == NEMA_ERR_IO);
    CHECK_EQ(starts_bad, 1);
    CHECK(strcmp(pal_desktops_active(), "") == 0); /* no other desktop was started in its place */
    CHECK(pal_console_front());
    char buf[24] = "";
    psys_settings_get("desktop.env", buf, sizeof(buf));
    CHECK(strcmp(buf, "b") == 0); /* a failed switch does not overwrite the saved choice */
    pal_console_hide();
    CHECK(pal_console_front()); /* nothing to give the screen to */
    pal_key_t k = switch_key();
    CHECK(pal_console_key(&k));
    CHECK(pal_console_front());
}

TEST(stop_leaves_the_console_and_use_gives_the_screen_back) {
    CHECK(pal_desktops_use("a") == NEMA_OK);
    pal_console_hide();
    CHECK(!pal_console_front());
    int stops = stops_a;
    pal_desktops_stop();
    CHECK_EQ(stops_a, stops + 1);
    CHECK(strcmp(pal_desktops_active(), "") == 0);
    CHECK(pal_console_front());
}

TEST(boot_starts_the_saved_desktop_and_stays_at_the_console_otherwise) {
    CHECK(psys_settings_set("desktop.env", "a") == 0);
    pal_desktops_boot();
    CHECK(strcmp(pal_desktops_active(), "a") == 0);
    CHECK(psys_settings_set("desktop.env", "missing") == 0);
    pal_desktops_boot();
    CHECK(strcmp(pal_desktops_active(), "a") == 0); /* an unknown name changes nothing that runs, and says so on the console */
    CHECK(pal_console_front());
    pal_desktops_stop();
    CHECK(psys_settings_set("desktop.env", "a") == 0);
    CHECK(psys_settings_set("boot.target", "console") == 0);
    pal_desktops_boot();
    CHECK(strcmp(pal_desktops_active(), "") == 0); /* boot.target console: no session at all */
    CHECK(psys_settings_set("boot.target", "desktop") == 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_display_ops_t ops = {.width = 64, .height = 48, .present = present};
    pal_display_init(&ops);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    pal_console_init(true);
    RUN(registration_rejects_bad_and_duplicate_entries);
    RUN(use_switches_stops_the_old_one_and_keeps_the_choice);
    RUN(the_console_is_in_front_until_the_boot_hands_the_screen_to_the_session);
    RUN(a_desktop_that_fails_leaves_no_session_and_the_console_in_front);
    RUN(stop_leaves_the_console_and_use_gives_the_screen_back);
    RUN(boot_starts_the_saved_desktop_and_stays_at_the_console_otherwise);
    return TEST_DONE();
}
