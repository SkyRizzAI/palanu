/* Toasts (system/src/toast.c) with a fake clock and no desktop task: the queue of the notify module (4 entries), the shown
 * toast's timer, the default and clamped durations, and the rate limit per source. The desktop is not started in this
 * process, so the queue is only drained by the calls below. */
#include <string.h>

#include "nema/nema.h"
#include "pal/notify.h"
#include "pal/toast.h"
#include "test.h"

TEST(toast_is_shown_for_its_duration_then_the_next_one_takes_over) {
    CHECK(!pal_toast_showing());
    CHECK(psys_toast_post("ta", "first", 500) == NEMA_OK); /* one source per group: the rate limit is real time */
    CHECK(psys_toast_post("ta", "second", 300) == NEMA_OK);
    CHECK(pal_toast_service(1000)); /* takes "first" */
    CHECK(pal_toast_showing());
    CHECK(strcmp(pal_toast_text(), "first") == 0);
    CHECK_EQ(pal_toast_next_ms(1000), 500);
    CHECK(!pal_toast_service(1499)); /* still shown: nothing changed */
    CHECK_EQ(pal_toast_next_ms(1499), 1);
    CHECK(pal_toast_service(1500)); /* expired, "second" is taken at once */
    CHECK(strcmp(pal_toast_text(), "second") == 0);
    CHECK(pal_toast_service(1800)); /* second ends */
    CHECK(!pal_toast_showing());
    CHECK_EQ(pal_toast_next_ms(1800), -1); /* nothing armed: no polling */
    CHECK(!pal_toast_service(1900));
}

TEST(toast_default_duration_and_clamp) {
    CHECK(psys_toast("d", 0) == NEMA_OK);
    CHECK(pal_toast_service(0));
    CHECK_EQ(pal_toast_next_ms(0), PAL_TOAST_MS_DEFAULT);
    pal_toast_service(PAL_TOAST_MS_DEFAULT);
    CHECK(!pal_toast_showing());
    CHECK(psys_toast("low", 5) == NEMA_OK); /* clamped up to 100 ms */
    CHECK(pal_toast_service(10000));
    CHECK_EQ(pal_toast_next_ms(10000), 100);
    pal_toast_service(10100);
    CHECK(!pal_toast_showing());
}

TEST(toast_text_is_kept_whole_in_the_queue_and_cut_for_display) {
    char longtext[PSYS_NOTIFY_TEXT];
    memset(longtext, 'x', sizeof(longtext) - 1);
    longtext[sizeof(longtext) - 1] = 0;
    CHECK(psys_toast_post("tc", longtext, 200) == NEMA_OK);
    CHECK(pal_toast_service(20000));
    CHECK_EQ((int)strlen(pal_toast_text()), PAL_TOAST_CHARS);
    pal_toast_service(20200);
}

TEST(queue_holds_four_and_refuses_the_fifth) {
    /* Four sources, one post each (the rate limit is per source), fill the queue; a fifth source is refused as FULL. */
    CHECK(psys_toast_post("qa", "1", 300) == NEMA_OK);
    CHECK(psys_toast_post("qb", "2", 300) == NEMA_OK);
    CHECK(psys_toast_post("qc", "3", 300) == NEMA_OK);
    CHECK(psys_toast_post("qd", "4", 300) == NEMA_OK);
    CHECK(psys_toast_post("qe", "5", 300) == NEMA_ERR_FULL);
    CHECK(psys_toast_post("qa", "6", 300) == NEMA_ERR_FULL); /* qa is under its rate, the queue is full */
    /* Drain: four toasts appear in order. */
    uint32_t t = 30000;
    const char *want[4] = {"1", "2", "3", "4"};
    for (int i = 0; i < 4; i++) {
        CHECK(pal_toast_service(t));
        CHECK(strcmp(pal_toast_text(), want[i]) == 0);
        t += 300;
    }
    CHECK(pal_toast_service(t)); /* the last one ends */
    CHECK(!pal_toast_showing());
}

TEST(toast_rate_limit_is_per_app_and_checked_for_system_posts) {
    /* Repeated posts from one source inside a second: the fifth is BUSY (the notify rate rule, shared by toasts). */
    psys_toast_t t;
    while (psys_toast_take(&t)) { /* the queue starts empty */
    }
    int busy = 0;
    for (int i = 0; i < 5; i++)
        if (psys_toast_post("rate5", "rate", 100) == NEMA_ERR_BUSY) busy++;
    CHECK_EQ(busy, 1);
    while (psys_toast_take(&t)) {
    }
    CHECK(psys_toast(NULL, 200) == NEMA_ERR_INVAL);
    CHECK(psys_toast("x", 20000) == NEMA_OK); /* clamped to 10000; this is the second "system" post of the run */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(toast_is_shown_for_its_duration_then_the_next_one_takes_over);
    RUN(toast_default_duration_and_clamp);
    RUN(toast_text_is_kept_whole_in_the_queue_and_cut_for_display);
    RUN(queue_holds_four_and_refuses_the_fifth);
    RUN(toast_rate_limit_is_per_app_and_checked_for_system_posts);
    return TEST_DONE();
}
