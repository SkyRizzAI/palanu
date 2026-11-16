/* Notifications and toasts: the ring, the toast queue, the rate limit, the ownership rules, and the sink, through
 * the psys_api_* functions with a native caller and through the system core. The rate limit is one second per app, so
 * the tests that post more than four times wait for the window to pass. */
#include <stdio.h>
#include <string.h>

#include "../../system/src/api_impl.h"
#include "nema/nema.h"
#include "pal/notify.h"
#include "test.h"

static api_ctx_t *NO_CTX = NULL;

static void wait_window(void) { nema_port_sleep_ms(1100); }

/* ---- the sink ---- */
static int sink_n[4]; /* POSTED, UPDATED, CANCELED, TOASTED */
static uint32_t sink_last_id;
static int sink_null_n;
static void sink(psys_notify_change_t change, const psys_notify_t *n) {
    sink_n[change]++;
    if (n)
        sink_last_id = n->id;
    else
        sink_null_n++;
}

TEST(post_update_cancel_and_the_list) {
    int32_t a = psys_api_notify_post(NO_CTX, "build", "done", 0);
    CHECK(a > 0);
    int32_t b = psys_api_notify_post(NO_CTX, "sync", "idle", PSYS_NOTIFY_ALERT);
    CHECK(b > a);
    CHECK_EQ(psys_notify_count(), 2);
    psys_notify_t list[4];
    CHECK_EQ(psys_notify_list(list, 4), 2);
    CHECK_EQ(list[0].id, (uint32_t)b); /* newest first */
    CHECK(strcmp(list[0].title, "sync") == 0 && strcmp(list[0].text, "idle") == 0);
    CHECK(strcmp(list[0].source, "system") == 0);
    CHECK_EQ(list[0].flags, PSYS_NOTIFY_ALERT);
    CHECK(!list[0].read);
    CHECK_EQ(psys_api_notify_update(NO_CTX, a, "build", "passed"), NEMA_OK);
    CHECK_EQ(psys_notify_list(list, 4), 2);
    CHECK(strcmp(list[1].text, "passed") == 0);
    CHECK_EQ(psys_api_notify_cancel(NO_CTX, b), NEMA_OK);
    CHECK_EQ(psys_api_notify_cancel(NO_CTX, b), NEMA_ERR_NOENT);
    CHECK_EQ(psys_api_notify_update(NO_CTX, b, "x", "y"), NEMA_ERR_NOENT);
    CHECK_EQ(psys_notify_count(), 1);
    psys_notify_mark_all_read();
    CHECK_EQ(psys_notify_list(list, 4), 1);
    CHECK(list[0].read);
    psys_notify_clear();
    CHECK_EQ(psys_notify_count(), 0);
}

TEST(a_notification_is_checked_on_the_way_in) {
    char t32[33], x96[97];
    memset(t32, 't', 32);
    t32[32] = 0;
    memset(x96, 'x', 96);
    x96[96] = 0;
    CHECK_EQ(psys_notify_post("system", t32, "x", 0), NEMA_ERR_INVAL);
    CHECK_EQ(psys_notify_post("system", "t", x96, 0), NEMA_ERR_INVAL);
    CHECK_EQ(psys_notify_post("system", "t", "x", 2), NEMA_ERR_INVAL); /* only PAL_NOTIFY_ALERT is defined */
    CHECK_EQ(psys_notify_post("system", "t", "x", -1), NEMA_ERR_INVAL);
    CHECK_EQ(psys_notify_post("system", NULL, "x", 0), NEMA_ERR_INVAL);
    psys_notify_clear();
}

TEST(a_full_title_and_text_are_kept) {
    char t31[32], x95[96];
    memset(t31, 'T', 31);
    t31[31] = 0;
    memset(x95, 'X', 95);
    x95[95] = 0;
    int32_t id = psys_notify_post("full", t31, x95, 0);
    CHECK(id > 0);
    psys_notify_t list[1];
    CHECK_EQ(psys_notify_list(list, 1), 1);
    CHECK(strcmp(list[0].title, t31) == 0 && strcmp(list[0].text, x95) == 0);
    psys_notify_clear();
}

TEST(another_apps_notification_is_not_touched) {
    int32_t id = psys_notify_post("owner-a", "a", "a", 0);
    CHECK(id > 0);
    CHECK_EQ(psys_notify_update("owner-b", id, "hijack", "hijack"), NEMA_ERR_PERM);
    CHECK_EQ(psys_notify_cancel("owner-b", id), NEMA_ERR_PERM);
    CHECK_EQ(psys_notify_count(), 1);
    CHECK_EQ(psys_notify_cancel("owner-a", id), NEMA_OK);
}

TEST(the_ring_keeps_the_last_32) {
    psys_notify_clear();
    char src[8], txt[8];
    int32_t first = -1, last = -1;
    /* forty posts, four per source, in ten sources: each source is under its own rate limit */
    for (int s = 0; s < 10; s++) {
        snprintf(src, sizeof(src), "ring%d", s);
        for (int k = 0; k < 4; k++) {
            snprintf(txt, sizeof(txt), "%d", s * 4 + k);
            int32_t id = psys_notify_post(src, "r", txt, 0);
            CHECK(id > 0);
            if (first < 0) first = id;
            last = id;
        }
    }
    CHECK_EQ(psys_notify_count(), PSYS_NOTIFY_RING);
    psys_notify_t list[PSYS_NOTIFY_RING];
    CHECK_EQ(psys_notify_list(list, PSYS_NOTIFY_RING), PSYS_NOTIFY_RING);
    CHECK_EQ(list[0].id, (uint32_t)last);
    CHECK_EQ(list[PSYS_NOTIFY_RING - 1].id, (uint32_t)(last - PSYS_NOTIFY_RING + 1));
    CHECK(last - first + 1 == 40);
    psys_notify_clear();
}

TEST(each_app_gets_four_posts_a_second) {
    psys_notify_clear();
    for (int i = 0; i < 4; i++) CHECK(psys_notify_post("rate", "r", "x", 0) > 0);
    CHECK_EQ(psys_notify_post("rate", "r", "x", 0), NEMA_ERR_BUSY);
    /* the toast of the same app counts with its posts */
    CHECK_EQ(psys_toast_post("rate", "hi", 500), NEMA_ERR_BUSY);
    /* another app is not held back by it */
    CHECK(psys_notify_post("other", "o", "x", 0) > 0);
    wait_window();
    CHECK(psys_notify_post("rate", "r", "x", 0) > 0);
    psys_notify_clear();
}

TEST(toasts_queue_four_and_come_out_in_order) {
    CHECK_EQ(psys_toast_post("tA", "one", 500), NEMA_OK);
    CHECK_EQ(psys_toast_post("tA", "two", 500), NEMA_OK);
    CHECK_EQ(psys_toast_post("tB", "three", 500), NEMA_OK);
    CHECK_EQ(psys_toast_post("tB", "four", 500), NEMA_OK);
    CHECK_EQ(psys_toast_post("tC", "five", 500), NEMA_ERR_FULL);
    psys_toast_t t;
    CHECK_EQ(psys_toast_take(&t), 1);
    CHECK(strcmp(t.text, "one") == 0 && strcmp(t.source, "tA") == 0 && t.ms == 500);
    CHECK_EQ(psys_toast_take(&t), 1);
    CHECK(strcmp(t.text, "two") == 0);
    CHECK_EQ(psys_toast_take(&t), 1);
    CHECK(strcmp(t.text, "three") == 0);
    CHECK_EQ(psys_toast_take(&t), 1);
    CHECK(strcmp(t.text, "four") == 0);
    CHECK_EQ(psys_toast_take(&t), 0);
}

TEST(toast_duration_is_checked) {
    CHECK_EQ(psys_toast_post("tD", "x", 99), NEMA_ERR_INVAL);
    CHECK_EQ(psys_toast_post("tD", "x", 10001), NEMA_ERR_INVAL);
    CHECK_EQ(psys_toast_post("tD", "x", 100), NEMA_OK);
    CHECK_EQ(psys_toast_post("tD", "x", 10000), NEMA_OK);
    psys_toast_t t;
    while (psys_toast_take(&t)) {
    }
}

TEST(the_sink_hears_every_change) {
    memset(sink_n, 0, sizeof(sink_n));
    sink_null_n = 0;
    psys_notify_set_sink(sink);
    int32_t id = psys_api_notify_post(NO_CTX, "sk", "p", 0);
    CHECK_EQ(sink_n[PSYS_NOTIFY_POSTED], 1);
    CHECK_EQ(sink_last_id, (uint32_t)id);
    CHECK_EQ(psys_api_notify_update(NO_CTX, id, "p2", "q"), NEMA_OK);
    CHECK_EQ(sink_n[PSYS_NOTIFY_UPDATED], 1);
    CHECK_EQ(psys_api_notify_cancel(NO_CTX, id), NEMA_OK);
    CHECK_EQ(sink_n[PSYS_NOTIFY_CANCELED], 1);
    CHECK_EQ(sink_last_id, (uint32_t)id);
    CHECK_EQ(psys_api_toast(NO_CTX, "t", 500), NEMA_OK);
    CHECK_EQ(sink_n[PSYS_NOTIFY_TOASTED], 1);
    CHECK_EQ(sink_null_n, 1); /* a toast comes without a notification */
    /* a refused post reaches nobody */
    CHECK_EQ(psys_notify_post("sk", "p", "x", 4), NEMA_ERR_INVAL);
    CHECK_EQ(sink_n[PSYS_NOTIFY_POSTED], 1);
    psys_notify_set_sink(NULL);
    psys_api_notify_post(NO_CTX, "sk2", "p", 0);
    CHECK_EQ(sink_n[PSYS_NOTIFY_POSTED], 1);
    psys_notify_clear();
    psys_toast_t t;
    while (psys_toast_take(&t)) {
    }
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(post_update_cancel_and_the_list);
    RUN(a_notification_is_checked_on_the_way_in);
    RUN(a_full_title_and_text_are_kept);
    RUN(another_apps_notification_is_not_touched);
    RUN(the_ring_keeps_the_last_32);
    RUN(each_app_gets_four_posts_a_second);
    RUN(toasts_queue_four_and_come_out_in_order);
    RUN(toast_duration_is_checked);
    RUN(the_sink_hears_every_change);
    return TEST_DONE();
}
