/* The Logs app's model (apps/logs_model.c): the line format, the uptime stamp, the clear marker, the merge in time order, the level and
 * component filters, the component list, follow mode, and a real line from the nema log ring. */
#include <string.h>

#include "logs_model.h"
#include "nema/nema.h"
#include "nema/log.h"
#include "test.h"

#define LINE(s) (s), strlen(s)

TEST(parse_reads_level_time_component_and_message) {
    logs_line_t ln;
    CHECK(logs_parse(LINE("W (1234) radio-wifi: retrying in 5 s"), &ln));
    CHECK_EQ(ln.level, 1);
    CHECK_EQ(ln.ms, 1234);
    CHECK(strcmp(ln.comp, "radio-wifi") == 0);
    CHECK(ln.msglen == strlen("retrying in 5 s"));
    CHECK(memcmp(ln.msg, "retrying", 8) == 0);
    CHECK(logs_parse(LINE("E (0) boot: x"), &ln) && ln.level == 0);
    CHECK(logs_parse(LINE("I (7) a: b"), &ln) && ln.level == 2);
    CHECK(logs_parse(LINE("D (9) debug: y"), &ln) && ln.level == 3);
    CHECK(!logs_parse(LINE("X (1) a: b"), &ln));  /* unknown level */
    CHECK(!logs_parse(LINE("I 1 a: b"), &ln));    /* no parenthesis */
    CHECK(!logs_parse(LINE("I (1) no colon"), &ln));
    CHECK(!logs_parse(LINE("I ()"), &ln));
}

TEST(hms_is_the_uptime_clock) {
    char s[16];
    logs_hms(0, s, sizeof(s));
    CHECK(strcmp(s, "00:00:00") == 0);
    logs_hms(3723000u, s, sizeof(s));
    CHECK(strcmp(s, "01:02:03") == 0);
}

TEST(marker_hides_lines_up_to_it) {
    const char ring[] = "I (1) a: first\nW (2) b: second\nE (3) c: third\n";
    CHECK_EQ(logs_after_marker(ring, strlen(ring), "W (2) b: second"), 31);
    CHECK_EQ(logs_after_marker(ring, strlen(ring), "E (3) c: third"), strlen(ring));
    CHECK_EQ(logs_after_marker(ring, strlen(ring), "gone"), 0);
    CHECK_EQ(logs_after_marker(ring, strlen(ring), ""), 0);
    /* a marker must be a whole line: a suffix of another line does not match */
    CHECK_EQ(logs_after_marker(ring, strlen(ring), "second"), 0);
}

static logs_ref_t REFS[LOGS_MAX];

TEST(merge_sorts_by_time_and_keeps_the_newest) {
    const char ring[] = "I (30) wifi: late\nE (10) ssh: early\nW (20) radio: middle\n";
    int n = logs_add_lines(REFS, 0, LOGS_MAX, LINE(ring), 0, LOGS_SRC_RING);
    CHECK_EQ(n, 3);
    const char svc[] = "I (25) svc-a: from service\n";
    n = logs_add_lines(REFS, n, LOGS_MAX, LINE(svc), 0, LOGS_SRC_SVC);
    logs_sort(REFS, n);
    CHECK_EQ(REFS[0].ms, 10);
    CHECK_EQ(REFS[1].ms, 20);
    CHECK_EQ(REFS[2].ms, 25);
    CHECK_EQ(REFS[2].src, LOGS_SRC_SVC);
    CHECK_EQ(REFS[3].ms, 30);
    /* the cap drops the oldest arrival: of three lines, the first one goes */
    n = logs_add_lines(REFS, 0, 2, LINE(ring), 0, LOGS_SRC_RING);
    CHECK_EQ(n, 2);
    CHECK_EQ(REFS[0].ms, 10);
    CHECK_EQ(REFS[1].ms, 20);
}

TEST(filters_by_level_and_component) {
    const char ring[] = "E (1) ssh: a\nW (2) wifi: b\nI (3) wifi: c\nD (4) wifi: d\nI (5) boot: e\n";
    int n = logs_add_lines(REFS, 0, LOGS_MAX, LINE(ring), 0, LOGS_SRC_RING);
    int vis[LOGS_MAX];
    CHECK_EQ(logs_visible(REFS, n, LOGS_F_ALL, NULL, vis, LOGS_MAX), 5);
    CHECK_EQ(logs_visible(REFS, n, LOGS_F_INFO, NULL, vis, LOGS_MAX), 4);
    CHECK_EQ(logs_visible(REFS, n, LOGS_F_WARN, NULL, vis, LOGS_MAX), 2);
    CHECK_EQ(logs_visible(REFS, n, LOGS_F_ERROR, NULL, vis, LOGS_MAX), 1);
    CHECK_EQ(logs_visible(REFS, n, LOGS_F_ALL, "wifi", vis, LOGS_MAX), 3);
    CHECK_EQ(logs_visible(REFS, n, LOGS_F_INFO, "wifi", vis, LOGS_MAX), 2);
    CHECK_EQ(logs_visible(REFS, n, LOGS_F_ALL, "nope", vis, LOGS_MAX), 0);
    CHECK(strcmp(logs_filter_name(LOGS_F_WARN), "Warn+") == 0);
    CHECK(strcmp(logs_filter_name(LOGS_F_ERROR), "Error") == 0);
}

TEST(component_list_is_distinct_in_first_seen_order) {
    const char ring[] = "I (1) wifi: a\nI (2) ssh: b\nI (3) wifi: c\nI (4) boot: d\n";
    int n = logs_add_lines(REFS, 0, LOGS_MAX, LINE(ring), 0, LOGS_SRC_RING);
    char names[8][LOGS_COMP];
    int k = logs_components(REFS, n, names, 8);
    CHECK_EQ(k, 3);
    CHECK(strcmp(names[0], "wifi") == 0);
    CHECK(strcmp(names[1], "ssh") == 0);
    CHECK(strcmp(names[2], "boot") == 0);
}

TEST(follow_keeps_the_last_rows_in_view) {
    CHECK_EQ(logs_follow_top(100, 10), 90);
    CHECK_EQ(logs_follow_top(4, 10), 0);
    CHECK_EQ(logs_follow_top(0, 10), 0);
}

TEST(reads_the_nema_ring_written_by_nema_log) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    NEMA_LOGW("logs-test", "disk is %d%% full", 91);
    NEMA_LOGE("logs-test", "write failed: %s", "no space");
    char buf[4096];
    size_t n = nema_log_snapshot(buf, sizeof(buf));
    int found = 0;
    static logs_ref_t refs[LOGS_MAX];
    int m = logs_add_lines(refs, 0, LOGS_MAX, buf, n, 0, LOGS_SRC_RING);
    for (int i = 0; i < m; i++) {
        if (strcmp(refs[i].comp, "logs-test") != 0) continue;
        found++;
        if (refs[i].level == 1) {
            logs_line_t ln;
            CHECK(logs_parse(buf + refs[i].off, refs[i].len, &ln));
            CHECK(ln.msglen == strlen("disk is 91% full"));
        }
    }
    CHECK_EQ(found, 2);
}

int main(void) {
    RUN(parse_reads_level_time_component_and_message);
    RUN(hms_is_the_uptime_clock);
    RUN(marker_hides_lines_up_to_it);
    RUN(merge_sorts_by_time_and_keeps_the_newest);
    RUN(filters_by_level_and_component);
    RUN(component_list_is_distinct_in_first_seen_order);
    RUN(follow_keeps_the_last_rows_in_view);
    RUN(reads_the_nema_ring_written_by_nema_log);
    return TEST_DONE();
}
