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

int main(void) {
    RUN(a_board_without_ops_keeps_its_clock);
    RUN(each_profile_is_a_cpu_range);
    return TEST_DONE();
}
