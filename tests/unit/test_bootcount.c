/* The boot-attempt counter and safe mode (system/src/bootcount.c), with a fake store. The platform's real store is
 * NVS on the board and a file on the host; the logic here is the same for both. */
#include <string.h>

#include "nema/nema.h"
#include "pal/bootcount.h"
#include "test.h"

static uint32_t g_stored;
static int g_sets;
static bool g_fail_set;

static uint32_t f_get(void) { return g_stored; }
static nema_err_t f_set(uint32_t n) {
    g_sets++;
    if (g_fail_set) return NEMA_ERR_IO;
    g_stored = n;
    return NEMA_OK;
}
static const pal_bootcount_ops_t FAKE = {f_get, f_set};

static void reset(uint32_t stored) {
    psys_bootcount_reset_for_test();
    g_stored = stored;
    g_sets = 0;
    g_fail_set = false;
}

static const uint64_t S = 1000000u; /* one second, in microseconds */

TEST(each_boot_adds_one_and_the_third_in_a_row_is_safe) {
    reset(0);
    CHECK(!psys_bootcount_begin(&FAKE));
    CHECK_EQ(g_stored, 1);
    CHECK_EQ(psys_bootcount_value(), 1);
    reset(g_stored);
    CHECK(!psys_bootcount_begin(&FAKE)); /* the second attempt */
    CHECK_EQ(g_stored, 2);
    reset(g_stored);
    CHECK(psys_bootcount_begin(&FAKE)); /* the third: safe mode */
    CHECK(psys_bootcount_safe());
    CHECK_EQ(g_stored, 3);
    CHECK_EQ(psys_bootcount_value(), 3);
}

TEST(a_stored_count_of_two_makes_the_next_boot_safe) {
    reset(2);
    CHECK(psys_bootcount_begin(&FAKE));
    CHECK_EQ(g_stored, 3);
}

TEST(a_ready_boot_clears_the_count_thirty_seconds_after_ready) {
    reset(0);
    psys_bootcount_begin(&FAKE);
    psys_bootcount_ready_check(0, false); /* services still starting */
    CHECK(!psys_bootcount_is_ready());
    psys_bootcount_ready_check(5 * S, true); /* settled at 5 s: the 30 s window starts now */
    CHECK(psys_bootcount_is_ready());
    psys_bootcount_ready_check(34 * S, true);
    CHECK_EQ(g_stored, 1); /* 29 s after ready: not yet */
    psys_bootcount_ready_check(35 * S, true);
    CHECK_EQ(g_stored, 0); /* 30 s after ready */
}

TEST(a_safe_boot_keeps_the_count_until_safemode_off) {
    reset(2);
    CHECK(psys_bootcount_begin(&FAKE));
    psys_bootcount_ready_check(0, true);
    psys_bootcount_ready_check(600 * S, true);
    CHECK_EQ(g_stored, 3); /* a safe boot never clears itself */
    CHECK(psys_bootcount_safe());
    CHECK_EQ(psys_bootcount_clear(), NEMA_OK); /* `safemode off` */
    CHECK_EQ(g_stored, 0);
    CHECK(psys_bootcount_safe()); /* this boot stays safe; the next one is normal */
}

TEST(a_refused_store_is_reported_not_hidden) {
    reset(0);
    g_fail_set = true;
    CHECK(!psys_bootcount_begin(&FAKE)); /* the boot goes on, unsafe, without a count */
    CHECK_EQ(psys_bootcount_clear(), NEMA_ERR_IO);
}

TEST(no_store_means_no_counting_and_never_safe) {
    reset(0);
    CHECK(!psys_bootcount_begin(NULL));
    CHECK(!psys_bootcount_safe());
    psys_bootcount_ready_check(0, true);
    psys_bootcount_ready_check(100 * S, true);
    CHECK_EQ(g_sets, 0);
    CHECK_EQ(psys_bootcount_clear(), NEMA_ERR_NOSYS);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(each_boot_adds_one_and_the_third_in_a_row_is_safe);
    RUN(a_stored_count_of_two_makes_the_next_boot_safe);
    RUN(a_ready_boot_clears_the_count_thirty_seconds_after_ready);
    RUN(a_safe_boot_keeps_the_count_until_safemode_off);
    RUN(a_refused_store_is_reported_not_hidden);
    RUN(no_store_means_no_counting_and_never_safe);
    return TEST_DONE();
}
