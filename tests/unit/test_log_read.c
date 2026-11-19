/* The kernel log's reader by position (nema_log_read), what the log-file service uses: it gets each byte once, in order, and
 * learns how much the ring dropped when it fell behind. */
#include <string.h>

#include "nema/log.h"
#include "nema/nema.h"
#include "test.h"

TEST(reads_what_came_after_its_position_once) {
    uint64_t pos = nema_log_total();
    NEMA_LOGI("t", "one");
    NEMA_LOGI("t", "two");
    char buf[256];
    uint64_t lost = 1;
    size_t n = nema_log_read(&pos, buf, sizeof(buf) - 1, &lost);
    buf[n] = 0;
    CHECK(n > 0 && lost == 0);
    CHECK(strstr(buf, "t: one\n") != NULL && strstr(buf, "t: two\n") != NULL);
    CHECK(strstr(buf, "one") < strstr(buf, "two"));
    CHECK_EQ(pos, nema_log_total());
    CHECK_EQ((int)nema_log_read(&pos, buf, sizeof(buf), &lost), 0); /* nothing new */
}

TEST(a_small_buffer_reads_in_pieces) {
    uint64_t pos = nema_log_total();
    NEMA_LOGI("t", "abcdefghijklmnopqrstuvwxyz");
    char a[10], all[128];
    size_t k = 0, n;
    while ((n = nema_log_read(&pos, a, sizeof(a), NULL)) > 0) {
        memcpy(all + k, a, n);
        k += n;
    }
    all[k] = 0;
    CHECK(strstr(all, "t: abcdefghijklmnopqrstuvwxyz\n") != NULL);
}

TEST(falling_behind_reports_what_the_ring_dropped) {
    uint64_t pos = nema_log_total();
    for (int i = 0; i < 40; i++) NEMA_LOGI("t", "filler line number %d to wrap the small ring", i); /* > the 1 KB ring */
    char buf[2048];
    uint64_t lost = 0;
    size_t n = nema_log_read(&pos, buf, sizeof(buf), &lost);
    CHECK(lost > 0);
    CHECK(n <= 1024);
    CHECK_EQ(pos, nema_log_total());
}

int main(void) {
    nema_config_t cfg = {.console_log = false, .log_ring_bytes = 1024};
    nema_init(&cfg);
    RUN(reads_what_came_after_its_position_once);
    RUN(a_small_buffer_reads_in_pieces);
    RUN(falling_behind_reports_what_the_ring_dropped);
    return TEST_DONE();
}
