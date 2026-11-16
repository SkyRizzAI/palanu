#include <stdint.h>
#include <string.h>

#include "pal/perf.h"
#include "test.h"

TEST(empty_ring_reads_zero) {
    pal_perf_ring_t r = {0};
    CHECK_EQ(pal_perf_ring_percentile(&r, 50), 0);
    CHECK_EQ(pal_perf_ring_percentile(&r, 95), 0);
    CHECK_EQ(pal_perf_ring_max(&r), 0);
}

TEST(nearest_rank_percentiles_over_1_to_100) {
    pal_perf_ring_t r = {0};
    for (uint32_t v = 100; v >= 1; v--) pal_perf_ring_push(&r, v); /* pushed in reverse: order must not matter */
    CHECK_EQ(r.n, 100);
    CHECK_EQ(pal_perf_ring_percentile(&r, 50), 50);
    CHECK_EQ(pal_perf_ring_percentile(&r, 95), 95);
    CHECK_EQ(pal_perf_ring_percentile(&r, 100), 100);
    CHECK_EQ(pal_perf_ring_percentile(&r, 0), 1);
    CHECK_EQ(pal_perf_ring_max(&r), 100);
}

TEST(small_sample_rounds_rank_up) {
    pal_perf_ring_t r = {0};
    for (uint32_t v = 1; v <= 10; v++) pal_perf_ring_push(&r, v * 1000);
    CHECK_EQ(pal_perf_ring_percentile(&r, 50), 5000);  /* ceil(0.5 * 10) = 5th */
    CHECK_EQ(pal_perf_ring_percentile(&r, 95), 10000); /* ceil(9.5) = 10th */
    CHECK_EQ(pal_perf_ring_percentile(&r, 10), 1000);
}

TEST(single_sample_is_every_percentile) {
    pal_perf_ring_t r = {0};
    pal_perf_ring_push(&r, 4242);
    CHECK_EQ(pal_perf_ring_percentile(&r, 50), 4242);
    CHECK_EQ(pal_perf_ring_percentile(&r, 95), 4242);
    CHECK_EQ(pal_perf_ring_max(&r), 4242);
}

TEST(ring_keeps_only_last_128) {
    pal_perf_ring_t r = {0};
    /* 300 pushes: the first 172 are overwritten, so the valid values are 173..300 */
    for (uint32_t v = 1; v <= 300; v++) pal_perf_ring_push(&r, v);
    CHECK_EQ(r.n, PAL_PERF_LAT_RING);
    CHECK_EQ(pal_perf_ring_max(&r), 300);
    CHECK_EQ(pal_perf_ring_percentile(&r, 0), 173);
    CHECK_EQ(pal_perf_ring_percentile(&r, 50), 236); /* ceil(0.5 * 128) = 64th of 173..300 */
    CHECK_EQ(pal_perf_ring_percentile(&r, 95), 294); /* ceil(0.95 * 128) = 122nd */
}

TEST(percentile_clamps_pct_above_100) {
    pal_perf_ring_t r = {0};
    pal_perf_ring_push(&r, 7);
    pal_perf_ring_push(&r, 9);
    CHECK_EQ(pal_perf_ring_percentile(&r, 500), 9);
}

/* Boot stages are reported relative to the first mark, with the delta from the previous stage. The marks take an
 * explicit clock here so the test does not depend on the host's monotonic clock (which counts from host boot). */
TEST(boot_report_is_relative_to_first_mark) {
    char buf[256];
    size_t n = pal_boot_report(buf, sizeof(buf));
    (void)n;
    pal_boot_mark_at("start", 50552716u);
    pal_boot_mark_at("kernel", 50552730u);
    pal_boot_mark_at("display", 50552790u);
    n = pal_boot_report(buf, sizeof(buf));
    CHECK(strstr(buf, "start") != NULL);
    CHECK(strstr(buf, "kernel") != NULL);
    CHECK(strstr(buf, "       0 ms") != NULL); /* start is the origin */
    CHECK(strstr(buf, "      14 ms  (+14)") != NULL);
    CHECK(strstr(buf, "      74 ms  (+60)") != NULL);
    CHECK(strstr(buf, "50552716") == NULL);  /* no absolute uptime leaks into the report */
}

int main(void) {
    RUN(empty_ring_reads_zero);
    RUN(nearest_rank_percentiles_over_1_to_100);
    RUN(small_sample_rounds_rank_up);
    RUN(single_sample_is_every_percentile);
    RUN(ring_keeps_only_last_128);
    RUN(percentile_clamps_pct_above_100);
    RUN(boot_report_is_relative_to_first_mark);
    return TEST_DONE();
}
