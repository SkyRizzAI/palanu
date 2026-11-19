/* Events the system posts to apps (system/src/app_events.h, api/palanu/events.h): an app that opened its queue gets NOTIFY and
 * SETTING posted to every app and FOCUS posted to it alone, in order; a full queue keeps the newest 8; an app without a queue gets
 * nothing. The app is a native process that runs the API calls as an app would. */
#include <stdio.h>
#include <string.h>

#include "../../system/src/api_ctx.h"
#include "../../system/src/api_impl.h"
#include "../../system/src/app_events.h"
#include "nema/nema.h"
#include "pal/papp.h"
#include "test.h"

static volatile const void *g_ctx;   /* the app's context, for psys_events_post_app */
static volatile int g_ready;          /* the app opened its queue */
static uint32_t g_got[12][4];
static volatile int g_ngot, g_done;

static int evapp_main(int argc, char **argv) {
    (void)argc, (void)argv;
    pal_papp_t app;
    memset(&app, 0, sizeof(app));
    snprintf(app.id, sizeof(app.id), "evapp");
    app.api = 7;
    api_ctx_t *c = api_ctx_new(&app, argc, argv);
    if (!c) return 98;
    if (psys_api_events(c) < 0) return 97;
    g_ctx = c;
    g_ready = 1;
    int want = argc > 1 ? argv[1][0] - '0' : 3;
    uint8_t ev[16];
    for (int i = 0; i < want; i++) {
        if (psys_api_event_next(c, ev, sizeof(ev), 3000) != 16) break;
        for (int k = 0; k < 4; k++) g_got[g_ngot][k] = (uint32_t)(ev[4 * k] | ev[4 * k + 1] << 8 | ev[4 * k + 2] << 16 | ev[4 * k + 3] << 24);
        g_ngot++;
    }
    g_ctx = NULL;
    api_ctx_free(c);
    g_done = 1;
    return 0;
}

static const nema_app_t EVAPP = {"evapp", "app events (tests)", evapp_main, 0, NEMA_APP_HIDDEN};

static nema_pid_t start(const char *count) {
    g_ready = g_ngot = g_done = 0;
    const char *av[] = {"evapp", count};
    nema_spawn_t sp = {.name = "evapp", .argc = 2, .argv = av};
    nema_pid_t pid;
    if (nema_proc_spawn(&sp, &pid) != NEMA_OK) return -1;
    for (int i = 0; i < 200 && !g_ready; i++) nema_sleep_ms(5);
    return pid;
}

TEST(posted_events_arrive_in_order_to_the_right_apps) {
    nema_pid_t pid = start("3");
    CHECK(pid > 0 && g_ready);
    psys_events_post_all(6, 42, 0, 0);                              /* NOTIFY to every app */
    psys_events_post_app((const void *)0x1, 3, 0, 0, 0);            /* FOCUS to another app: not this one */
    psys_events_post_app((const void *)g_ctx, 3, 0, 0, 0);          /* FOCUS to this one */
    psys_events_post_all(7, psys_events_key_hash("ui.theme"), 0, 0); /* SETTING */
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 5000) == NEMA_OK);
    CHECK_EQ(g_ngot, 3);
    CHECK(g_got[0][0] == 6 && g_got[0][1] == 42);
    CHECK(g_got[1][0] == 3);
    CHECK(g_got[2][0] == 7 && g_got[2][1] == psys_events_key_hash("ui.theme"));
}

TEST(a_full_queue_keeps_the_newest_eight) {
    nema_pid_t pid = start("9");
    CHECK(pid > 0 && g_ready);
    for (uint32_t i = 1; i <= 10; i++) psys_events_post_app((const void *)g_ctx, 6, i, 0, 0);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 8000) == NEMA_OK);
    CHECK_EQ(g_ngot, 8); /* the ninth wait times out: two were dropped */
    CHECK_EQ((int)g_got[0][1], 3);
    CHECK_EQ((int)g_got[7][1], 10);
}

TEST(the_key_hash_is_fnv1a) {
    CHECK_EQ(psys_events_key_hash(""), 2166136261u);
    CHECK_EQ(psys_events_key_hash("a"), 0xE40C292Cu);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_app_register(&EVAPP);
    psys_events_post_all(6, 1, 0, 0); /* no app has a queue yet: nothing happens */
    RUN(posted_events_arrive_in_order_to_the_right_apps);
    RUN(a_full_queue_keeps_the_newest_eight);
    RUN(the_key_hash_is_fnv1a);
    return TEST_DONE();
}
