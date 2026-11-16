/* The OTA health-check policy (system/src/ota_confirm.c). The policy is a pure function of the health and the elapsed
 * time, so the host tests use fakes: a healthy board confirms, a board missing a check rolls back at the end of the 60 s
 * window and says why, and a late recovery inside the window still confirms. The reboot itself is the platform's ops
 * (ota_esp.c) and is checked on the board. */
#include <string.h>

#include "nema/nema.h"
#include "pal/ota_confirm.h"
#include "test.h"

static psys_ota_health_t H(bool frame, bool link, bool radios) {
    psys_ota_health_t h = {frame, link, radios};
    return h;
}

TEST(a_healthy_board_confirms_at_once) {
    char why[96] = "";
    CHECK_EQ(psys_ota_decide(&(psys_ota_health_t){true, true, true}, 0, why, sizeof(why)), PSYS_OTA_CONFIRM);
    CHECK_EQ(why[0], 0);
}

TEST(a_missing_check_waits_until_the_window_ends) {
    psys_ota_health_t no_frame = H(false, true, true);
    CHECK_EQ(psys_ota_decide(&no_frame, 0, NULL, 0), PSYS_OTA_WAIT);
    CHECK_EQ(psys_ota_decide(&no_frame, PSYS_OTA_WINDOW_MS - 1, NULL, 0), PSYS_OTA_WAIT);
    CHECK_EQ(psys_ota_decide(&no_frame, PSYS_OTA_WINDOW_MS, NULL, 0), PSYS_OTA_ROLLBACK);
}

TEST(the_rollback_says_which_checks_failed) {
    char why[96] = "";
    psys_ota_health_t none = H(false, false, true);
    CHECK_EQ(psys_ota_decide(&none, PSYS_OTA_WINDOW_MS, why, sizeof(why)), PSYS_OTA_ROLLBACK);
    CHECK(strstr(why, "no desktop frame") != NULL);
    CHECK(strstr(why, "link-uart not running") != NULL);
    CHECK(strstr(why, "radio") == NULL);

    psys_ota_health_t radio = H(true, true, false);
    CHECK_EQ(psys_ota_decide(&radio, PSYS_OTA_WINDOW_MS + 5000, why, sizeof(why)), PSYS_OTA_ROLLBACK);
    CHECK(strstr(why, "a radio failed") != NULL);
    CHECK(strstr(why, "desktop") == NULL);
}

TEST(a_recovery_inside_the_window_still_confirms) {
    psys_ota_health_t early = H(false, false, true);
    CHECK_EQ(psys_ota_decide(&early, 10000, NULL, 0), PSYS_OTA_WAIT);
    psys_ota_health_t later = H(true, true, true);
    CHECK_EQ(psys_ota_decide(&later, 20000, NULL, 0), PSYS_OTA_CONFIRM);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(a_healthy_board_confirms_at_once);
    RUN(a_missing_check_waits_until_the_window_ends);
    RUN(the_rollback_says_which_checks_failed);
    RUN(a_recovery_inside_the_window_still_confirms);
    return TEST_DONE();
}
