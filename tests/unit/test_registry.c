#include "nema/nema.h"
#include "test.h"

static int g_thing = 5;

static void publisher(void *arg) {
    (void)arg;
    nema_port_sleep_ms(60);
    nema_registry_publish("late", &g_thing);
}

TEST(publish_and_get) {
    void *p = NULL;
    CHECK(nema_registry_publish("display", &g_thing) == NEMA_OK);
    CHECK(nema_registry_publish("display", &g_thing) == NEMA_ERR_EXIST);
    CHECK(nema_registry_get("display", &p, 0) == NEMA_OK);
    CHECK(p == &g_thing);
    CHECK(nema_registry_unpublish("display") == NEMA_OK);
    CHECK(nema_registry_get("display", &p, 0) == NEMA_ERR_NOENT);
}

TEST(get_times_out) {
    void *p;
    uint64_t t0 = nema_port_time_us();
    CHECK(nema_registry_get("ghost", &p, 40) == NEMA_ERR_NOENT);
    CHECK(nema_port_time_us() - t0 >= 35000);
}

TEST(get_waits_for_a_later_publish) {
    nema_task_desc_t d = {.name = "pub", .fn = publisher, .stack = 4096, .role = NEMA_TASK_SYSTEM};
    CHECK(nema_task_create(&d) == NEMA_OK);
    void *p = NULL;
    CHECK(nema_registry_get("late", &p, 2000) == NEMA_OK);
    CHECK(p == &g_thing);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(publish_and_get);
    RUN(get_times_out);
    RUN(get_waits_for_a_later_publish);
    return TEST_DONE();
}
