/* Shell history across restarts (system/src/history.c): what is kept, the ring of 100, the idle delay before a write, the
 * file read back after a "restart" (the module reset and loaded again), long and odd lines in the file, and a failed write
 * that waits before it tries again. The store is a host directory mounted at "/". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nema/nema.h"
#include "pal/history.h"
#include "test.h"

static char ROOT[256];
static uint64_t T_US;
static uint64_t clock_us(void) { return T_US; }

static void restart(const char *file) { pal_history_reset_for_test(file, clock_us); }

static void write_host(const char *rel, const char *text) {
    char p[400];
    snprintf(p, sizeof(p), "%s%s", ROOT, rel);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
}

TEST(keeps_real_lines_only) {
    restart("/h1");
    pal_history_add("ls");
    pal_history_add("");
    pal_history_add("   ");
    pal_history_add(" secret"); /* leading space: not remembered */
    pal_history_add("ls");      /* a repeat of the last */
    pal_history_add("pwd");
    pal_history_add("ls");      /* not the last any more: kept */
    CHECK_EQ(pal_history_count(), 3);
    char b[32];
    CHECK(pal_history_get(0, b, sizeof(b)) && strcmp(b, "ls") == 0);
    CHECK(pal_history_get(1, b, sizeof(b)) && strcmp(b, "pwd") == 0);
    CHECK(pal_history_get(2, b, sizeof(b)) && strcmp(b, "ls") == 0);
    CHECK(!pal_history_get(3, b, sizeof(b)));
    CHECK(!pal_history_get(-1, b, sizeof(b)));
}

TEST(ring_keeps_the_newest_100) {
    restart("/h2");
    char l[16];
    for (int i = 0; i < 130; i++) {
        snprintf(l, sizeof(l), "cmd %d", i);
        pal_history_add(l);
    }
    CHECK_EQ(pal_history_count(), PAL_HISTORY_MAX);
    char b[16];
    CHECK(pal_history_get(0, b, sizeof(b)) && strcmp(b, "cmd 30") == 0);
    CHECK(pal_history_get(99, b, sizeof(b)) && strcmp(b, "cmd 129") == 0);
}

TEST(write_waits_for_a_quiet_moment) {
    restart("/sh/h3");
    T_US = 1000000;
    CHECK_EQ(pal_history_flush_in_ms(), -1); /* nothing to write */
    pal_history_add("make");
    CHECK_EQ(pal_history_flush_in_ms(), PAL_HISTORY_IDLE_MS + 1);
    T_US += 1500000;
    pal_history_add("make test"); /* the quiet period starts again */
    int32_t w = pal_history_flush_in_ms();
    CHECK(w > 1500 && w <= PAL_HISTORY_IDLE_MS + 1);
    T_US += (uint64_t)PAL_HISTORY_IDLE_MS * 1000u;
    CHECK_EQ(pal_history_flush_in_ms(), 0);
    CHECK_EQ(pal_history_flush(), NEMA_OK); /* also makes the missing /sh directory */
    CHECK_EQ(pal_history_flush_in_ms(), -1);
}

TEST(lines_come_back_after_a_restart) {
    restart("/sh/h3"); /* the file the last test wrote */
    CHECK_EQ(pal_history_count(), 2);
    char b[32];
    CHECK(pal_history_get(1, b, sizeof(b)) && strcmp(b, "make test") == 0);
    pal_history_add("ssh root@192.168.1.101");
    T_US += 10000000;
    CHECK_EQ(pal_history_flush(), NEMA_OK);
    restart("/sh/h3");
    CHECK_EQ(pal_history_count(), 3);
    CHECK(pal_history_get(2, b, sizeof(b)) && strcmp(b, "ssh root@192.168.1.101") == 0);
}

TEST(file_with_long_blank_and_crlf_lines) {
    char big[400];
    memset(big, 'x', sizeof(big) - 2);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = 0;
    char text[600];
    snprintf(text, sizeof(text), "one\r\n\n   \n%stwo", big); /* no newline at the end */
    write_host("/h5", text);
    restart("/h5");
    CHECK_EQ(pal_history_count(), 2); /* the over-long line is dropped, blanks skipped */
    char b[32];
    CHECK(pal_history_get(0, b, sizeof(b)) && strcmp(b, "one") == 0);
    CHECK(pal_history_get(1, b, sizeof(b)) && strcmp(b, "two") == 0);
}

TEST(failed_write_waits_before_trying_again) {
    restart("/missing-parent/deeper/h6"); /* the directory cannot be made in one step */
    T_US = 50000000;
    pal_history_add("x");
    T_US += (uint64_t)PAL_HISTORY_IDLE_MS * 1000u;
    CHECK(pal_history_flush() != NEMA_OK);
    int32_t w = pal_history_flush_in_ms();
    CHECK(w > PAL_HISTORY_IDLE_MS && w <= PAL_HISTORY_RETRY_MS + 1);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    const char *tmp = getenv("TMPDIR");
    snprintf(ROOT, sizeof(ROOT), "%s/palanu-hist-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(ROOT)) abort();
    nema_vfs_mount("/", nema_posixfs_ops(), nema_posixfs_create(ROOT));
    RUN(keeps_real_lines_only);
    RUN(ring_keeps_the_newest_100);
    RUN(write_waits_for_a_quiet_moment);
    RUN(lines_come_back_after_a_restart);
    RUN(file_with_long_blank_and_crlf_lines);
    RUN(failed_write_waits_before_trying_again);
    return TEST_DONE();
}
