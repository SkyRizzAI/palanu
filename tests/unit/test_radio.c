/* Radio manager (system/src/radio.c): on-demand start, holders, grace period, admission, failed starts, force off,
 * and a thread smoke test. The board's ops are fakes; the memory readings are a settable seam. */
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "nema/nema.h"
#include "pal/radio.h"
#include "test.h"

#define KB 1024u

static int s_starts, s_stops, s_bad;
static bool s_fail_start;
static size_t s_free, s_block;

static nema_err_t f_start(pal_radio_t r) {
    (void)r;
    __atomic_fetch_add(&s_starts, 1, __ATOMIC_SEQ_CST);
    return s_fail_start ? NEMA_ERR_IO : NEMA_OK;
}

static nema_err_t f_stop(pal_radio_t r) {
    (void)r;
    __atomic_fetch_add(&s_stops, 1, __ATOMIC_SEQ_CST);
    return NEMA_OK;
}

static void f_mem(size_t *free_bytes, size_t *largest) {
    if (free_bytes) *free_bytes = s_free;
    if (largest) *largest = s_block;
}

static const pal_radio_ops_t FAKE = {f_start, f_stop, NULL};

static void reset(void) {
    pal_radio_set_settle_ms(0); /* tests that look at the pause set it themselves */
    pal_radio_force_off(PAL_RADIO_WIFI);
    pal_radio_force_off(PAL_RADIO_BLE);
    pal_radio_set_grace_ms(0);
    s_free = 200u * KB;
    s_block = 100u * KB;
    s_fail_start = false;
    s_starts = 0;
    s_stops = 0;
    s_bad = 0;
}

static int holders_of(pal_radio_t r) {
    pal_radio_info_t in;
    pal_radio_info(r, &in);
    return in.nholders;
}

TEST(boots_off_and_starts_once_for_many_holders) {
    reset();
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_OFF);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "boot"), NEMA_OK);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_ON);
    CHECK_EQ(s_starts, 1);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "user"), NEMA_OK);
    CHECK_EQ(s_starts, 1);
    CHECK_EQ(holders_of(PAL_RADIO_WIFI), 2);
    CHECK_EQ(pal_radio_release(PAL_RADIO_WIFI, "boot"), NEMA_OK);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_ON);
    CHECK_EQ(s_stops, 0);
    CHECK_EQ(pal_radio_release(PAL_RADIO_WIFI, "user"), NEMA_OK);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_OFF);
    CHECK_EQ(s_stops, 1);
    CHECK_EQ(pal_radio_release(PAL_RADIO_WIFI, "user"), NEMA_ERR_NOENT);
}

TEST(same_holder_is_counted_once) {
    reset();
    CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, "boot"), NEMA_OK);
    CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, "boot"), NEMA_OK);
    CHECK_EQ(holders_of(PAL_RADIO_BLE), 1);
    CHECK_EQ(s_starts, 1);
    pal_radio_force_off(PAL_RADIO_BLE);
}

TEST(holder_table_is_bounded) {
    reset();
    char name[PAL_RADIO_HOLDER_LEN];
    for (int i = 0; i < PAL_RADIO_HOLDERS; i++) {
        snprintf(name, sizeof(name), "h%d", i);
        CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, name), NEMA_OK);
    }
    CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, "extra"), NEMA_ERR_FULL);
    CHECK_EQ(holders_of(PAL_RADIO_BLE), PAL_RADIO_HOLDERS);
    pal_radio_force_off(PAL_RADIO_BLE);
    CHECK_EQ(holders_of(PAL_RADIO_BLE), 0);
}

TEST(grace_period_holds_the_radio_and_a_new_request_cancels_it) {
    reset();
    pal_radio_set_grace_ms(300);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "boot"), NEMA_OK);
    CHECK_EQ(pal_radio_release(PAL_RADIO_WIFI, "boot"), NEMA_OK);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_ON);
    pal_radio_info_t in;
    pal_radio_info(PAL_RADIO_WIFI, &in);
    CHECK(in.grace_pending);
    nema_sleep_ms(100);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "user"), NEMA_OK);
    pal_radio_info(PAL_RADIO_WIFI, &in);
    CHECK(!in.grace_pending);
    CHECK_EQ(s_starts, 1);
    CHECK_EQ(pal_radio_release(PAL_RADIO_WIFI, "user"), NEMA_OK);
    nema_sleep_ms(150);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_ON); /* still inside the grace period */
    nema_sleep_ms(900);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_OFF);
    CHECK_EQ(s_stops, 1);
    CHECK_EQ(s_starts, 1);
    pal_radio_set_grace_ms(0);
}

TEST(admission_refuses_when_free_memory_is_short) {
    reset();
    /* WIFI needs its cost + the 32 KB floor free: leave it 4 KB short, while BLE (the cheaper one) still fits */
    s_free = (unsigned)(pal_radio_cost_kb(PAL_RADIO_WIFI) + 32 - 4) * KB;
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "boot"), NEMA_ERR_NOMEM);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_OFF);
    CHECK_EQ(s_starts, 0);
    CHECK(strstr(pal_radio_last_error(PAL_RADIO_WIFI), "refused") != NULL);
    /* BLE needs less than WIFI, so it still fits */
    CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, "boot"), NEMA_OK);
    CHECK_EQ(pal_radio_state(PAL_RADIO_BLE), PAL_RADIO_ON);
    CHECK_EQ(pal_radio_last_error(PAL_RADIO_BLE)[0], 0);
    pal_radio_force_off(PAL_RADIO_BLE);
}

TEST(admission_refuses_when_no_large_block_is_left) {
    reset();
    s_block = 10u * KB; /* WIFI needs a 24 KB block */
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "boot"), NEMA_ERR_NOMEM);
    CHECK_EQ(s_starts, 0);
    s_block = 24u * KB;
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "boot"), NEMA_OK);
    pal_radio_force_off(PAL_RADIO_WIFI);
}

TEST(failed_start_reports_failed_and_retry_is_allowed) {
    reset();
    s_fail_start = true;
    CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, "user"), NEMA_ERR_IO);
    CHECK_EQ(pal_radio_state(PAL_RADIO_BLE), PAL_RADIO_FAILED);
    CHECK(strstr(pal_radio_last_error(PAL_RADIO_BLE), "failed to start") != NULL);
    CHECK_EQ(holders_of(PAL_RADIO_BLE), 0);
    CHECK_EQ(s_stops, 1); /* the half-started radio was taken down */
    s_fail_start = false;
    CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, "user"), NEMA_OK);
    CHECK_EQ(pal_radio_state(PAL_RADIO_BLE), PAL_RADIO_ON);
    CHECK_EQ(pal_radio_last_error(PAL_RADIO_BLE)[0], 0);
    CHECK_EQ(s_starts, 2);
    pal_radio_force_off(PAL_RADIO_BLE);
}

TEST(force_off_drops_holders_and_stops_now) {
    reset();
    pal_radio_set_grace_ms(10000);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "boot"), NEMA_OK);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "user"), NEMA_OK);
    CHECK_EQ(pal_radio_request(PAL_RADIO_BLE, "user"), NEMA_OK);
    pal_radio_force_off(PAL_RADIO_WIFI);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_OFF);
    CHECK_EQ(holders_of(PAL_RADIO_WIFI), 0);
    CHECK_EQ(pal_radio_state(PAL_RADIO_BLE), PAL_RADIO_ON); /* the other radio is untouched */
    CHECK_EQ(s_stops, 1);
    pal_radio_force_off(PAL_RADIO_BLE);
    pal_radio_set_grace_ms(0);
}

TEST(bad_arguments_are_refused) {
    reset();
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, ""), NEMA_ERR_INVAL);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, NULL), NEMA_ERR_INVAL);
    CHECK_EQ(pal_radio_request(PAL_RADIO_WIFI, "0123456789abcdef"), NEMA_ERR_INVAL); /* 16 chars: too long */
    CHECK_EQ(pal_radio_request((pal_radio_t)7, "x"), NEMA_ERR_INVAL);
    CHECK_EQ(pal_radio_release(PAL_RADIO_WIFI, "nobody"), NEMA_ERR_NOENT);
    CHECK_EQ(s_starts, 0);
}

TEST(names_and_costs) {
    CHECK(strcmp(pal_radio_name(PAL_RADIO_WIFI), "wifi") == 0);
    CHECK(strcmp(pal_radio_name(PAL_RADIO_BLE), "ble") == 0);
    CHECK(strcmp(pal_radio_state_name(PAL_RADIO_FAILED), "failed") == 0);
    CHECK(pal_radio_cost_kb(PAL_RADIO_WIFI) > pal_radio_cost_kb(PAL_RADIO_BLE));
    CHECK(pal_radio_cost_kb(PAL_RADIO_BLE) > 0);
}

static const char *NAMES[4] = {"t0", "t1", "t2", "t3"};

static void *worker(void *arg) {
    const char *name = arg;
    for (int i = 0; i < 200; i++) {
        if (pal_radio_request(PAL_RADIO_WIFI, name) != NEMA_OK) __atomic_fetch_add(&s_bad, 1, __ATOMIC_SEQ_CST);
        if (pal_radio_release(PAL_RADIO_WIFI, name) != NEMA_OK) __atomic_fetch_add(&s_bad, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

TEST(threads_request_and_release_without_losing_a_holder) {
    reset();
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker, (void *)NAMES[i]);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK_EQ(s_bad, 0);
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_OFF);
    CHECK_EQ(holders_of(PAL_RADIO_WIFI), 0);
    CHECK_EQ(s_starts, s_stops); /* every start was paired with a stop */
    CHECK(s_starts > 0);
}

/* ---- the two radios never start in the same instant (the boot crash of 2026-11-14) ---- */

static volatile int s_in_flight, s_max_in_flight;
static uint64_t s_op_start_us[PAL_RADIO_COUNT], s_op_end_us[PAL_RADIO_COUNT];
static bool s_slow;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static nema_err_t slow_start(pal_radio_t r) {
    int now_in = __atomic_add_fetch(&s_in_flight, 1, __ATOMIC_SEQ_CST);
    if (now_in > s_max_in_flight) s_max_in_flight = now_in;
    s_op_start_us[r] = now_us();
    nema_sleep_ms(60); /* esp_wifi_start and the NimBLE host take longer than this on the board */
    s_op_end_us[r] = now_us();
    __atomic_fetch_sub(&s_in_flight, 1, __ATOMIC_SEQ_CST);
    return NEMA_OK;
}

static void *request_then_hold(void *arg) {
    pal_radio_request(*(pal_radio_t *)arg, "boot");
    return NULL;
}

TEST(starts_of_the_two_radios_do_not_overlap_and_settle_between) {
    static const pal_radio_ops_t SLOW = {slow_start, f_stop, NULL};
    reset();
    pal_radio_set_ops(&SLOW);
    pal_radio_set_settle_ms(200);
    s_in_flight = s_max_in_flight = 0;
    pal_radio_t w = PAL_RADIO_WIFI, b = PAL_RADIO_BLE;
    pthread_t t1, t2;
    pthread_create(&t1, NULL, request_then_hold, &w); /* both at once, as the supervisor and a service start at boot */
    pthread_create(&t2, NULL, request_then_hold, &b);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    CHECK_EQ(s_max_in_flight, 1); /* one bring-up at a time */
    uint64_t first_end = s_op_end_us[0] < s_op_end_us[1] ? s_op_end_us[0] : s_op_end_us[1];
    uint64_t second_start = s_op_start_us[0] < s_op_start_us[1] ? s_op_start_us[1] : s_op_start_us[0];
    CHECK(second_start >= first_end + 190000u); /* and the second began only after the settle pause (200 ms, a little slack) */
    CHECK_EQ(pal_radio_state(PAL_RADIO_WIFI), PAL_RADIO_ON);
    CHECK_EQ(pal_radio_state(PAL_RADIO_BLE), PAL_RADIO_ON);
    pal_radio_set_ops(&FAKE);
    (void)s_slow;
}

TEST(a_radio_started_long_after_the_other_does_not_wait) {
    static const pal_radio_ops_t SLOW = {slow_start, f_stop, NULL};
    reset();
    pal_radio_set_ops(&SLOW);
    pal_radio_set_settle_ms(150);
    pal_radio_request(PAL_RADIO_WIFI, "a");
    nema_sleep_ms(250); /* longer than the settle pause: nothing is owed */
    uint64_t t0 = now_us();
    pal_radio_request(PAL_RADIO_BLE, "b");
    uint64_t took = now_us() - t0;
    CHECK(took < 130000u); /* about the 60 ms start itself, not 60 + a pause */
    pal_radio_set_ops(&FAKE);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_radio_set_ops(&FAKE);
    pal_radio_set_mem_source(f_mem);
    RUN(boots_off_and_starts_once_for_many_holders);
    RUN(starts_of_the_two_radios_do_not_overlap_and_settle_between);
    RUN(a_radio_started_long_after_the_other_does_not_wait);
    RUN(same_holder_is_counted_once);
    RUN(holder_table_is_bounded);
    RUN(grace_period_holds_the_radio_and_a_new_request_cancels_it);
    RUN(admission_refuses_when_free_memory_is_short);
    RUN(admission_refuses_when_no_large_block_is_left);
    RUN(failed_start_reports_failed_and_retry_is_allowed);
    RUN(force_off_drops_holders_and_stops_now);
    RUN(bad_arguments_are_refused);
    RUN(names_and_costs);
    RUN(threads_request_and_release_without_losing_a_holder);
    return TEST_DONE();
}
