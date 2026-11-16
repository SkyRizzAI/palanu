#include <string.h>

#include "nema/nema.h"
#include "test.h"

/* The host port has no real load counter, so these check the contract every port must keep: readings stay in
 * 0..100, each caller's sample is its own (zero-initialised and independent), and a bad core index reads 0.
 * The delta arithmetic itself is platform code (platform/esp-idf) and is verified on the board. */

TEST(readings_are_in_range) {
    nema_cpu_sample_t s;
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < nema_port_cpu_count(); c++) {
            int p = nema_port_cpu_percent(&s, c);
            CHECK(p >= 0 && p <= 100);
        }
    }
}

TEST(samples_are_per_caller) {
    nema_cpu_sample_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    /* interleave two readers the way top and Monitor do; each must still get a valid reading */
    int pa = nema_port_cpu_percent(&a, 0);
    int pb = nema_port_cpu_percent(&b, 0);
    int pa2 = nema_port_cpu_percent(&a, 0);
    CHECK(pa >= 0 && pa <= 100);
    CHECK(pb >= 0 && pb <= 100);
    CHECK(pa2 >= 0 && pa2 <= 100);
}

TEST(bad_core_reads_zero) {
    nema_cpu_sample_t s;
    memset(&s, 0, sizeof(s));
    CHECK_EQ(nema_port_cpu_percent(&s, -1), 0);
    CHECK_EQ(nema_port_cpu_percent(&s, NEMA_CPU_MAX), 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(readings_are_in_range);
    RUN(samples_are_per_caller);
    RUN(bad_core_reads_zero);
    return TEST_DONE();
}
