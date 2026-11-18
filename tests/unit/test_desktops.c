/* The desktop registry (system/src/desktops.c): registration rules, switching with stop and start, the fallback to "classic" when a
 * desktop will not start, and the choice kept in `desktop.env`. No desktop task runs in this process. */
#include <string.h>

#include "nema/nema.h"
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
static const pal_desktop_env_t CLASSIC = {.id = "classic", .title = "Classic", .about = "fallback", .start = start_classic};

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
    CHECK(pal_desktops_register(&CLASSIC) == NEMA_OK); /* registered last on purpose: the fallback is found by its id */
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

TEST(a_desktop_that_fails_leaves_the_classic_one_running) {
    CHECK(pal_desktops_use("bad") == NEMA_ERR_IO);
    CHECK_EQ(starts_bad, 1);
    CHECK(strcmp(pal_desktops_active(), "classic") == 0);
    char buf[24] = "";
    psys_settings_get("desktop.env", buf, sizeof(buf));
    CHECK(strcmp(buf, "b") == 0); /* a failed switch does not overwrite the saved choice */
}

TEST(boot_starts_the_saved_desktop_and_falls_back_on_an_unknown_one) {
    CHECK(psys_settings_set("desktop.env", "a") == 0);
    pal_desktops_boot();
    CHECK(strcmp(pal_desktops_active(), "a") == 0);
    CHECK(psys_settings_set("desktop.env", "missing") == 0);
    pal_desktops_boot();
    CHECK(strcmp(pal_desktops_active(), "classic") == 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_display_ops_t ops = {.width = 64, .height = 48, .present = present};
    pal_display_init(&ops);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    RUN(registration_rejects_bad_and_duplicate_entries);
    RUN(use_switches_stops_the_old_one_and_keeps_the_choice);
    RUN(a_desktop_that_fails_leaves_the_classic_one_running);
    RUN(boot_starts_the_saved_desktop_and_falls_back_on_an_unknown_one);
    return TEST_DONE();
}
