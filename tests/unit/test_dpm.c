/* The display power manager (system/src/dpm.c) with a fake clock: sleep after idle, the first key after sleep wakes and is
 * consumed, activity resets the idle timer without waking, the lock after a further timeout, an unlock that turns the screen on,
 * and the PIN (salted SHA-256). The clock is only the number the test passes in, so no task runs here. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/dpm.h"
#include "test.h"

static int d_calls, d_last_on, lock_calls, lock_answer = 1;

static void hook_display(bool on) {
    d_calls++;
    d_last_on = on;
}
static bool hook_lock(void) {
    lock_calls++;
    return lock_answer != 0;
}

static void fresh(uint32_t now) {
    pal_dpm_hooks_t h = {hook_display, hook_lock};
    d_calls = 0;
    d_last_on = -1;
    lock_calls = 0;
    lock_answer = 1;
    pal_dpm_set_sleep_ms(PAL_DPM_SLEEP_DEFAULT_MS);
    pal_dpm_set_lock_ms(PAL_DPM_LOCK_DEFAULT_MS);
    pal_dpm_init(&h, now);
}

TEST(defaults_match_the_settings_table) {
    CHECK_EQ(PAL_DPM_SLEEP_DEFAULT_MS, 60000);
    CHECK_EQ(PAL_DPM_LOCK_DEFAULT_MS, 0);
    fresh(0);
    CHECK_EQ(pal_dpm_sleep_ms(), 60000);
    CHECK_EQ(pal_dpm_lock_ms(), 0);
    CHECK(pal_dpm_state() == PAL_DPM_ACTIVE);
    CHECK(pal_dpm_display_on());
}

TEST(idle_sleeps_the_display_at_the_timeout) {
    fresh(0);
    pal_dpm_tick(59999);
    CHECK(pal_dpm_display_on());
    CHECK_EQ(d_calls, 0);
    pal_dpm_tick(60000);
    CHECK(!pal_dpm_display_on());
    CHECK(pal_dpm_state() == PAL_DPM_SLEEP);
    CHECK_EQ(d_calls, 1);
    CHECK_EQ(d_last_on, 0);
    pal_dpm_tick(90000); /* stays asleep: the hook is not called again */
    CHECK_EQ(d_calls, 1);
}

TEST(first_key_after_sleep_wakes_and_is_consumed) {
    fresh(0);
    pal_dpm_tick(60000);
    CHECK(pal_dpm_key(61000));
    CHECK(pal_dpm_display_on());
    CHECK_EQ(d_last_on, 1);
    CHECK(pal_dpm_state() == PAL_DPM_ACTIVE);
    CHECK(!pal_dpm_key(61500)); /* awake: delivered normally */
}

TEST(a_key_resets_the_idle_timer) {
    fresh(0);
    CHECK(!pal_dpm_key(50000));
    pal_dpm_tick(109999);
    CHECK(pal_dpm_display_on());
    pal_dpm_tick(110000);
    CHECK(!pal_dpm_display_on());
}

TEST(activity_resets_idle_but_never_wakes) {
    fresh(0);
    pal_dpm_tick(60000);
    pal_dpm_activity(61000);
    CHECK(!pal_dpm_display_on()); /* Link input does not wake the screen */
    pal_dpm_set_locked(false, 61000); /* an unlock does, though */
    CHECK(pal_dpm_display_on());
}

TEST(zero_sleep_means_never) {
    fresh(0);
    pal_dpm_set_sleep_ms(0);
    pal_dpm_tick(10000000);
    CHECK(pal_dpm_display_on());
    CHECK_EQ(pal_dpm_next_ms(10000000), -1);
}

TEST(next_deadline_is_reported) {
    fresh(1000);
    CHECK_EQ(pal_dpm_next_ms(1000), 60000);
    CHECK_EQ(pal_dpm_next_ms(31000), 30000);
    pal_dpm_tick(61000);
    CHECK_EQ(pal_dpm_next_ms(61000), -1); /* asleep, no lock timeout: nothing armed */
}

TEST(lock_follows_sleep_after_its_own_timeout) {
    fresh(0);
    pal_dpm_set_lock_ms(10000);
    pal_dpm_tick(60000); /* asleep at 60000 */
    pal_dpm_tick(69999);
    CHECK_EQ(lock_calls, 0);
    CHECK(pal_dpm_state() == PAL_DPM_SLEEP);
    pal_dpm_tick(70000);
    CHECK_EQ(lock_calls, 1);
    CHECK(pal_dpm_state() == PAL_DPM_LOCKED);
    CHECK(pal_dpm_locked());
    CHECK(!pal_dpm_display_on()); /* locked while the screen is off */
}

TEST(the_first_key_while_locked_wakes_and_is_consumed) {
    fresh(0);
    pal_dpm_set_lock_ms(10000);
    pal_dpm_tick(60000);
    pal_dpm_tick(70000);
    CHECK(pal_dpm_key(71000));
    CHECK(pal_dpm_display_on());
    CHECK(pal_dpm_locked()); /* waking does not unlock */
    CHECK(pal_dpm_state() == PAL_DPM_LOCKED);
    CHECK(!pal_dpm_key(71500)); /* later keys reach the lock view */
}

TEST(refused_lock_is_retried_after_another_lock_timeout) {
    fresh(0);
    pal_dpm_set_lock_ms(10000);
    lock_answer = 0;
    pal_dpm_tick(60000);
    pal_dpm_tick(70000);
    CHECK_EQ(lock_calls, 1);
    CHECK(!pal_dpm_locked());
    pal_dpm_tick(79999);
    CHECK_EQ(lock_calls, 1);
    lock_answer = 1;
    pal_dpm_tick(80000);
    CHECK_EQ(lock_calls, 2);
    CHECK(pal_dpm_locked());
}

TEST(a_locked_device_sleeps_its_screen_on_idle_and_stays_locked) {
    fresh(0);
    pal_dpm_set_locked(true, 0);
    CHECK(pal_dpm_state() == PAL_DPM_LOCKED);
    pal_dpm_tick(59999);
    CHECK(pal_dpm_display_on());
    pal_dpm_tick(60000);
    CHECK(!pal_dpm_display_on());
    CHECK(pal_dpm_locked());
    CHECK(pal_dpm_key(60500));
    CHECK(pal_dpm_locked());
}

TEST(unlock_turns_the_screen_on_and_restarts_idle) {
    fresh(0);
    pal_dpm_set_locked(true, 0);
    pal_dpm_tick(60000);
    pal_dpm_set_locked(false, 65000);
    CHECK(pal_dpm_display_on());
    CHECK(pal_dpm_state() == PAL_DPM_ACTIVE);
    pal_dpm_tick(124999);
    CHECK(pal_dpm_display_on());
    pal_dpm_tick(125000);
    CHECK(!pal_dpm_display_on());
}

TEST(settings_values_are_kept) {
    fresh(0);
    pal_dpm_set_sleep_ms(30000);
    pal_dpm_set_lock_ms(120000);
    CHECK_EQ(pal_dpm_sleep_ms(), 30000);
    CHECK_EQ(pal_dpm_lock_ms(), 120000);
    pal_dpm_hooks_t h = {hook_display, hook_lock};
    pal_dpm_init(&h, 5); /* re-initialising the hooks and timers keeps the settings */
    CHECK_EQ(pal_dpm_sleep_ms(), 30000);
    CHECK_EQ(pal_dpm_lock_ms(), 120000);
    pal_dpm_set_sleep_ms(PAL_DPM_SLEEP_DEFAULT_MS);
    pal_dpm_set_lock_ms(PAL_DPM_LOCK_DEFAULT_MS);
}

TEST(pin_check_and_text_round_trip) {
    CHECK(!pal_dpm_pin_is_set());
    CHECK(!pal_dpm_pin_check("1234"));
    CHECK(pal_dpm_pin_set("123") == NEMA_ERR_INVAL); /* shorter than four */
    CHECK(pal_dpm_pin_set("has space") == NEMA_ERR_INVAL);
    CHECK(pal_dpm_pin_set("1234") == NEMA_OK);
    CHECK(pal_dpm_pin_is_set());
    CHECK(pal_dpm_pin_check("1234"));
    CHECK(!pal_dpm_pin_check("1235"));
    CHECK(!pal_dpm_pin_check("12345"));

    char a[120], b[120];
    pal_dpm_pin_text(a, sizeof(a));
    CHECK(strlen(a) == 97); /* 16-byte salt and 32-byte hash in hex, with a colon */

    pal_dpm_pin_set("1234"); /* a new salt: the text changes */
    pal_dpm_pin_text(b, sizeof(b));
    CHECK(strcmp(a, b) != 0);

    CHECK(pal_dpm_pin_set("") == NEMA_OK);
    CHECK(!pal_dpm_pin_is_set());
    pal_dpm_pin_text(b, sizeof(b));
    CHECK(b[0] == 0);

    pal_dpm_pin_load(a); /* from the settings file */
    CHECK(pal_dpm_pin_is_set());
    CHECK(pal_dpm_pin_check("1234"));
    CHECK(!pal_dpm_pin_check("0000"));
    pal_dpm_pin_load("nonsense");
    CHECK(!pal_dpm_pin_is_set());
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(defaults_match_the_settings_table);
    RUN(idle_sleeps_the_display_at_the_timeout);
    RUN(first_key_after_sleep_wakes_and_is_consumed);
    RUN(a_key_resets_the_idle_timer);
    RUN(activity_resets_idle_but_never_wakes);
    RUN(zero_sleep_means_never);
    RUN(next_deadline_is_reported);
    RUN(lock_follows_sleep_after_its_own_timeout);
    RUN(the_first_key_while_locked_wakes_and_is_consumed);
    RUN(refused_lock_is_retried_after_another_lock_timeout);
    RUN(a_locked_device_sleeps_its_screen_on_idle_and_stays_locked);
    RUN(unlock_turns_the_screen_on_and_restarts_idle);
    RUN(settings_values_are_kept);
    RUN(pin_check_and_text_round_trip);
    return TEST_DONE();
}
