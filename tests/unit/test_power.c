/* Power profiles (pal/power.h): each profile's CPU range from the board's top clock, the description, a board without ops. */
#include <string.h>

#include "pal/power.h"
#include "test.h"

static int g_min, g_max, g_calls;
static nema_err_t cpu(int min_mhz, int max_mhz) {
    g_min = min_mhz, g_max = max_mhz, g_calls++;
    return NEMA_OK;
}

TEST(a_board_without_ops_keeps_its_clock) {
    CHECK_EQ(pal_power_apply(PAL_POWER_SAVER), NEMA_ERR_NOSYS);
    char d[64];
    pal_power_describe(d, sizeof(d));
    CHECK(strstr(d, "keeps its clock") != NULL);
    CHECK_EQ(pal_power_profile(), PAL_POWER_SAVER);
}

TEST(each_profile_is_a_cpu_range) {
    pal_power_ops_t ops = {.cpu = cpu, .max_mhz = 320};
    pal_power_set_ops(&ops);
    CHECK_EQ(pal_power_apply(PAL_POWER_PERFORMANCE), NEMA_OK);
    CHECK(g_min == 320 && g_max == 320);
    CHECK_EQ(pal_power_apply(PAL_POWER_BALANCED), NEMA_OK);
    CHECK(g_min == 80 && g_max == 320);
    CHECK_EQ(pal_power_apply(PAL_POWER_SAVER), NEMA_OK);
    CHECK(g_min == 40 && g_max == 160);
    char d[64];
    pal_power_describe(d, sizeof(d));
    CHECK(!strcmp(d, "Saver: CPU 40 to 160 MHz"));
    pal_power_apply(PAL_POWER_PERFORMANCE);
    pal_power_describe(d, sizeof(d));
    CHECK(!strcmp(d, "Performance: CPU 320 MHz"));
    CHECK_EQ(pal_power_apply(3), NEMA_ERR_INVAL);
    CHECK_EQ(g_calls, 4);
}

static int g_sleep = -1, g_sleep_calls;
static nema_err_t light(bool allow) {
    g_sleep = allow, g_sleep_calls++;
    return NEMA_OK;
}

TEST(saver_light_sleeps_only_with_the_screen_off) {
    pal_power_ops_t ops = {.cpu = cpu, .max_mhz = 320, .light_sleep = light};
    pal_power_set_ops(&ops);
    pal_power_screen(true);
    CHECK_EQ(pal_power_apply(PAL_POWER_SAVER), NEMA_OK);
    CHECK(!pal_power_sleeping()); /* the screen is on */
    pal_power_screen(false);
    CHECK(pal_power_sleeping() && g_sleep == 1);
    char d[80];
    pal_power_describe(d, sizeof(d));
    CHECK(!strcmp(d, "Saver: CPU 40 to 160 MHz, light sleep now"));
    pal_power_screen(true);
    CHECK(!pal_power_sleeping() && g_sleep == 0);
    pal_power_screen(false);
    CHECK_EQ(pal_power_apply(PAL_POWER_BALANCED), NEMA_OK); /* another profile: awake even with the screen off */
    CHECK(!pal_power_sleeping());
    int calls = g_sleep_calls;
    pal_power_screen(true);
    pal_power_screen(false);
    CHECK_EQ(g_sleep_calls, calls); /* Balanced never asks for it */
    pal_power_screen(true);
}

int main(void) {
    RUN(a_board_without_ops_keeps_its_clock);
    RUN(each_profile_is_a_cpu_range);
    RUN(saver_light_sleeps_only_with_the_screen_off);
    return TEST_DONE();
}
