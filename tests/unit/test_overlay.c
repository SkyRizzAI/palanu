/* The compositor's layers (system/src/overlay.c, desktop.c): overlay order, the keys (top overlay first, Esc pops it unless the
 * view takes it), obscured and exposed, the opaque overlay that stops lower redraws, the home key bindings, the switcher, the
 * lock (every key to the lock view, Esc does not pop it), the status item limit, desktop toasts, and the sleep wake key. The
 * desktop runs on its own task; keys go in through pal_input_inject like real presses. The display is 320x240 and its present
 * callback only counts. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/dpm.h"
#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/notify.h"
#include "pal/overlay.h"
#include "pal/toast.h"
#include "test.h"

extern const pal_keyboard_t HHV2_KEYBOARD;

typedef struct {
    volatile int enter, leave, draw, draw_full, key, pump, tick, obscured, exposed;
    volatile bool last_full, handle;
} fake_t;

enum { LAUNCH, A, OV1, OV2, CTL, NOTIF, LOCKV, NVIEWS };
static fake_t F[NVIEWS];
static pal_view_t V[NVIEWS];
static char IDS[NVIEWS][16];
static volatile int g_presents;

static void v_enter(pal_view_t *v) { ((fake_t *)v->data)->enter++; }
static void v_leave(pal_view_t *v) { ((fake_t *)v->data)->leave++; }
static void v_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    fake_t *f = (fake_t *)v->data;
    pal_fill(c, a.x, a.y, 1, 1, full ? 2 : 3);
    f->draw++;
    if (full) f->draw_full++;
    f->last_full = full;
}
static bool v_key(pal_view_t *v, const pal_key_t *k) {
    (void)k;
    fake_t *f = (fake_t *)v->data;
    f->key++;
    return f->handle;
}
static void v_pump(pal_view_t *v) { ((fake_t *)v->data)->pump++; }
static void v_tick(pal_view_t *v, uint32_t now) {
    (void)now;
    ((fake_t *)v->data)->tick++;
}
static void v_obscured(pal_view_t *v) { ((fake_t *)v->data)->obscured++; }
static void v_exposed(pal_view_t *v) { ((fake_t *)v->data)->exposed++; }

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    (void)ctx, (void)px, (void)stride, (void)r, (void)pal;
    g_presents++;
}

static void make_view(int i, const char *id, uint32_t flags, uint32_t tick_ms) {
    nema_strlcpy(IDS[i], id, sizeof(IDS[i]));
    V[i] = (pal_view_t){.id = IDS[i], .title = IDS[i], .flags = flags, .enter = v_enter, .leave = v_leave,
                        .draw = v_draw, .key = v_key, .pump = v_pump, .tick = tick_ms ? v_tick : NULL,
                        .tick_ms = tick_ms, .data = &F[i], .obscured = v_obscured, .exposed = v_exposed};
}

static void key(uint8_t sym) {
    pal_key_t k = {sym, 0, false, 0};
    pal_input_inject(&k);
}
static void key_char(uint32_t cp) {
    pal_key_t k = {PAL_KEY_CHAR, 0, false, cp};
    pal_input_inject(&k);
}
static void sleep_ms(int ms) { nema_port_sleep_ms((uint32_t)ms); }

/* Polls a condition for up to ms; the helpers below are the conditions. */
static bool wait_front(const char *id) {
    for (int t = 0; t < 2000; t += 5) {
        if (strcmp(pal_desktop_front_id(), id) == 0) return true;
        sleep_ms(5);
    }
    return strcmp(pal_desktop_front_id(), id) == 0;
}
static bool wait_top(const pal_view_t *v) {
    for (int t = 0; t < 2000; t += 5) {
        if (pal_desktop_overlay_top() == v) return true;
        sleep_ms(5);
    }
    return pal_desktop_overlay_top() == v;
}
static bool wait_count(size_t n) {
    for (int t = 0; t < 2000; t += 5) {
        if (pal_desktop_overlay_count() == n) return true;
        sleep_ms(5);
    }
    return pal_desktop_overlay_count() == n;
}
static bool wait_above(volatile int *counter, int above) {
    for (int t = 0; t < 2000; t += 5) {
        if (*counter > above) return true;
        sleep_ms(5);
    }
    return *counter > above;
}
static bool wait_until_false(bool (*cond)(void)) {
    for (int t = 0; t < 3000; t += 5) {
        if (!cond()) return true;
        sleep_ms(5);
    }
    return !cond();
}
static bool wait_until_true(bool (*cond)(void)) {
    for (int t = 0; t < 3000; t += 5) {
        if (cond()) return true;
        sleep_ms(5);
    }
    return cond();
}
static bool is_locked(void) { return pal_desktop_locked(); }
static bool display_is_on(void) { return pal_dpm_display_on(); }
static void settle(void) { sleep_ms(120); }

/* ---------------------------------------------------------------- before the task runs: the plain API */
TEST(push_rejects_null_and_duplicates_and_counts) {
    CHECK(pal_desktop_overlay_push(NULL) == NEMA_ERR_INVAL);
    pal_desktop_overlay_pop(); /* empty: nothing happens */
    CHECK_EQ(pal_desktop_overlay_count(), 0);
    CHECK(pal_desktop_overlay_top() == NULL);
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_ERR_EXIST);
    CHECK(pal_desktop_overlay_top() == &V[OV1]);
    CHECK(pal_desktop_overlay_push(&V[OV2]) == NEMA_OK);
    CHECK_EQ(pal_desktop_overlay_count(), 2);
    CHECK(pal_desktop_overlay_top() == &V[OV2]);
    pal_desktop_overlay_pop();
    CHECK(pal_desktop_overlay_top() == &V[OV1]);
    pal_desktop_overlay_pop();
    CHECK_EQ(pal_desktop_overlay_count(), 0);
    CHECK_EQ(F[OV1].leave, 1);
    CHECK_EQ(F[OV2].leave, 1);
}

TEST(overlay_stack_is_capped_at_four) {
    CHECK(pal_desktop_overlay_push(&V[A]) == NEMA_OK);
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(pal_desktop_overlay_push(&V[OV2]) == NEMA_OK);
    CHECK(pal_desktop_overlay_push(&V[CTL]) == NEMA_OK);
    CHECK(pal_desktop_overlay_push(&V[NOTIF]) == NEMA_ERR_FULL);
    CHECK_EQ(pal_desktop_overlay_count(), 4);
    for (int i = 0; i < 4; i++) pal_desktop_overlay_pop();
    CHECK_EQ(pal_desktop_overlay_count(), 0);
}

static int status_draw(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st) {
    (void)c, (void)t, (void)x, (void)h, (void)st;
    return 0;
}

TEST(status_items_are_limited_to_twelve) {
    /* The five built-in items (the keyboard modes, the unread dot, lock, Bluetooth, the card) are registered by the desktop at
     * init: seven more fit, the eighth does not. */
    static pal_status_item_t it[8];
    CHECK(pal_status_item_register(&it[0]) == NEMA_ERR_INVAL); /* no draw callback */
    for (int i = 0; i < 8; i++) {
        it[i].id = "t";
        it[i].draw = status_draw;
    }
    for (int i = 0; i < 7; i++) CHECK(pal_status_item_register(&it[i]) == NEMA_OK);
    CHECK(pal_status_item_register(&it[7]) == NEMA_ERR_FULL);
}

/* ---------------------------------------------------------------- with the task running */
TEST(task_starts_and_launcher_is_front) {
    CHECK(pal_desktop_start() == NEMA_OK);
    CHECK(wait_front("launcher"));
}

TEST(overlay_gets_keys_before_the_front_view) {
    F[OV1].handle = true;
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(wait_top(&V[OV1]));
    int lk = F[LAUNCH].key;
    key_char('q');
    CHECK(wait_above(&F[OV1].key, 0));
    settle();
    CHECK_EQ(F[LAUNCH].key, lk);
    pal_desktop_overlay_pop();
    CHECK(wait_count(0));
}

TEST(escape_pops_an_overlay_that_does_not_take_it) {
    int ex0 = F[LAUNCH].exposed;
    F[OV1].handle = false;
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(wait_top(&V[OV1]));
    int lk = F[LAUNCH].key;
    key(PAL_KEY_ESC);
    CHECK(wait_count(0));
    CHECK(F[OV1].leave >= 1);
    CHECK_EQ(F[LAUNCH].key, lk); /* the pop used the key up */
    CHECK(wait_above(&F[LAUNCH].exposed, ex0));
}

TEST(escape_stays_with_an_overlay_that_takes_it) {
    F[OV1].handle = true;
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(wait_top(&V[OV1]));
    int k0 = F[OV1].key;
    key(PAL_KEY_ESC);
    CHECK(wait_above(&F[OV1].key, k0));
    settle();
    CHECK(pal_desktop_overlay_top() == &V[OV1]);
    F[OV1].handle = false;
    pal_desktop_overlay_pop();
    CHECK(wait_count(0));
}

TEST(obscured_and_exposed_tell_the_layer_below) {
    int ob0 = F[LAUNCH].obscured, ex0 = F[LAUNCH].exposed;
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(wait_top(&V[OV1]));
    CHECK(wait_above(&F[LAUNCH].obscured, ob0));
    pal_desktop_overlay_pop();
    CHECK(wait_count(0));
    CHECK(wait_above(&F[LAUNCH].exposed, ex0));
}

TEST(opaque_overlay_stops_lower_layers_being_drawn) {
    settle();
    V[OV2].flags = PAL_VIEW_OPAQUE;
    int lfull0 = F[LAUNCH].draw_full, ov0 = F[OV2].draw, ovfull0 = F[OV2].draw_full;
    CHECK(pal_desktop_overlay_push(&V[OV2]) == NEMA_OK);
    CHECK(wait_above(&F[OV2].draw_full, ovfull0)); /* pushing draws the overlay in full */
    CHECK(wait_top(&V[OV2]));
    settle();
    CHECK_EQ(F[LAUNCH].draw_full, lfull0); /* the launcher is not drawn under an opaque overlay */
    int l0 = F[LAUNCH].draw;
    pal_desktop_invalidate();
    CHECK(wait_above(&F[OV2].draw, ov0));
    settle();
    CHECK_EQ(F[LAUNCH].draw, l0); /* a partial redraw draws the top only */
    pal_desktop_overlay_pop();
    CHECK(wait_count(0));
    CHECK(wait_above(&F[LAUNCH].draw_full, lfull0)); /* closing redraws the layer below in full */
    CHECK(F[LAUNCH].last_full);
    V[OV2].flags = 0;
}

TEST(partial_redraw_draws_only_the_top_overlay) {
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(wait_top(&V[OV1]));
    settle();
    int l0 = F[LAUNCH].draw, o0 = F[OV1].draw;
    pal_desktop_invalidate();
    CHECK(wait_above(&F[OV1].draw, o0));
    settle();
    CHECK_EQ(F[LAUNCH].draw, l0);
    pal_desktop_overlay_pop();
    CHECK(wait_count(0));
}

TEST(home_keys_open_bound_views_as_opaque_overlays) {
    CHECK(pal_desktop_bind_home_key(PAL_KEY_UP, "ctl") == NEMA_OK);
    CHECK(pal_desktop_bind_home_key(PAL_KEY_DOWN, "notif") == NEMA_OK);
    CHECK(wait_front("launcher"));
    key(PAL_KEY_UP);
    CHECK(wait_top(&V[CTL]));
    key(PAL_KEY_ESC); /* the control view does not take Esc */
    CHECK(wait_count(0));
    key(PAL_KEY_DOWN);
    CHECK(wait_top(&V[NOTIF]));
    key(PAL_KEY_ESC);
    CHECK(wait_count(0));
    CHECK(strcmp(pal_desktop_front_id(), "launcher") == 0);
}

TEST(home_binding_does_not_apply_away_from_home) {
    pal_desktop_open("a");
    CHECK(wait_front("a"));
    int ak = F[A].key;
    key(PAL_KEY_UP);
    CHECK(wait_above(&F[A].key, ak));
    CHECK_EQ(pal_desktop_overlay_count(), 0);
    pal_desktop_open("launcher");
    CHECK(wait_front("launcher"));
}

TEST(unknown_bound_view_and_unbinding) {
    CHECK(pal_desktop_bind_home_key(PAL_KEY_ENTER, "nope") == NEMA_OK);
    int lk = F[LAUNCH].key;
    key(PAL_KEY_ENTER);
    CHECK(wait_above(&F[LAUNCH].key, lk)); /* no such view: the launcher gets Enter */
    CHECK(pal_desktop_bind_home_key(PAL_KEY_ENTER, NULL) == NEMA_OK);
    CHECK(pal_desktop_bind_home_key(PAL_KEY_UP, NULL) == NEMA_OK);
    lk = F[LAUNCH].key;
    key(PAL_KEY_UP);
    CHECK(wait_above(&F[LAUNCH].key, lk));
    CHECK_EQ(pal_desktop_overlay_count(), 0);
}

TEST(switch_key_opens_the_switcher_and_enter_picks) {
    CHECK(wait_front("launcher"));
    key(PAL_KEY_SWITCH);
    CHECK(wait_count(1));
    CHECK(strcmp(pal_desktop_overlay_top()->id, "switcher") == 0);
    key(PAL_KEY_ESC); /* the switcher does not take Esc: it closes */
    CHECK(wait_count(0));
    CHECK(strcmp(pal_desktop_front_id(), "launcher") == 0);
    pal_desktop_open("a");
    CHECK(wait_front("a"));
    pal_desktop_home();
    CHECK(wait_front("launcher"));
    pal_desktop_open("a");
    CHECK(wait_front("a"));
    key(PAL_KEY_SWITCH); /* from a, the previous app is the launcher */
    CHECK(wait_count(1));
    key(PAL_KEY_ENTER);
    CHECK(wait_front("launcher"));
    CHECK(wait_count(0));
}

TEST(home_key_clears_the_overlays) {
    F[OV1].handle = true;
    CHECK(pal_desktop_overlay_push(&V[OV1]) == NEMA_OK);
    CHECK(wait_top(&V[OV1]));
    int l0 = F[OV1].leave;
    key(PAL_KEY_GOHOME);
    CHECK(wait_count(0));
    CHECK(wait_above(&F[OV1].leave, l0));
    CHECK(strcmp(pal_desktop_front_id(), "launcher") == 0);
    F[OV1].handle = false;
}

TEST(lock_routes_every_key_to_the_lock_view) {
    pal_desktop_set_lock_view("lockv");
    F[LOCKV].handle = false;
    CHECK(pal_desktop_lock() == NEMA_OK);
    CHECK(wait_until_true(is_locked));
    CHECK(wait_top(&V[LOCKV]));
    int lk = F[LAUNCH].key, lv = F[LOCKV].key;
    key_char('z');
    CHECK(wait_above(&F[LOCKV].key, lv));
    key(PAL_KEY_GOHOME); /* ignored while locked */
    settle();
    CHECK(strcmp(pal_desktop_front_id(), "launcher") == 0);
    pal_desktop_open("a"); /* refused while locked */
    settle();
    CHECK(strcmp(pal_desktop_front_id(), "launcher") == 0);
    key(PAL_KEY_ESC); /* the lock view does not pop on Esc */
    settle();
    CHECK(pal_desktop_overlay_top() == &V[LOCKV]);
    CHECK_EQ(F[LAUNCH].key, lk);
    CHECK(pal_desktop_locked());
    CHECK(pal_desktop_overlay_count() == 1);
    pal_desktop_unlock();
    CHECK(wait_until_false(is_locked));
    CHECK(wait_count(0));
    CHECK(pal_desktop_lock() == NEMA_OK);
    CHECK(wait_until_true(is_locked));
    pal_desktop_unlock();
    CHECK(wait_until_false(is_locked));
}

TEST(lock_needs_a_registered_lock_view) {
    /* With the desktop running, the lock is queued: the answer is the state after the desktop ran the request. */
    pal_desktop_set_lock_view("missing");
    CHECK(pal_desktop_lock() == NEMA_OK);
    settle();
    CHECK(!pal_desktop_locked());
    CHECK(pal_desktop_overlay_count() == 0);
    pal_desktop_set_lock_view("lockv");
}

TEST(desktop_toast_is_shown_and_removed_by_the_timer) {
    CHECK(psys_toast_post("tt", "hello desk", 300) == NEMA_OK);
    bool seen = false;
    for (int t = 0; t < 2000 && !seen; t += 5) {
        seen = pal_toast_showing();
        if (!seen) sleep_ms(5);
    }
    CHECK(seen);
    CHECK(strcmp(pal_toast_text(), "hello desk") == 0);
    bool gone = false;
    for (int t = 0; t < 2000 && !gone; t += 5) {
        gone = !pal_toast_showing();
        if (!gone) sleep_ms(5);
    }
    CHECK(gone);
}

static bool sleeping(void) { return !pal_dpm_display_on(); }

TEST(sleep_then_the_first_key_only_wakes_the_screen) {
    CHECK(wait_front("launcher"));
    pal_dpm_set_sleep_ms(1000);
    key(PAL_KEY_ESC); /* any key counts as activity; Esc is harmless on the launcher */
    settle();
    int lk = F[LAUNCH].key;
    CHECK(wait_until_true(sleeping));
    key_char('w'); /* the wake key: consumed, the launcher does not see it */
    CHECK(wait_until_true(display_is_on));
    settle();
    CHECK_EQ(F[LAUNCH].key, lk);
    key_char('w'); /* awake again: delivered */
    CHECK(wait_above(&F[LAUNCH].key, lk));
    pal_dpm_set_sleep_ms(PAL_DPM_SLEEP_DEFAULT_MS);
}

TEST(link_activity_resets_idle_and_does_not_wake) {
    pal_dpm_set_sleep_ms(1500);
    settle();
    pal_desktop_activity();
    sleep_ms(1000);
    pal_desktop_activity();
    sleep_ms(1000);
    CHECK(display_is_on()); /* kept awake by activity */
    pal_dpm_set_sleep_ms(PAL_DPM_SLEEP_DEFAULT_MS);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_input_init(&HHV2_KEYBOARD);
    pal_display_ops_t ops = {.width = 320, .height = 240, .present = present, .backlight = NULL, .ctx = NULL};
    pal_display_init(&ops);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    make_view(LAUNCH, "launcher", PAL_VIEW_HIDDEN, 50);
    make_view(A, "a", 0, 50);
    make_view(OV1, "ov1", 0, 0);
    make_view(OV2, "ov2", 0, 0);
    make_view(CTL, "ctl", 0, 0);
    make_view(NOTIF, "notif", 0, 0);
    make_view(LOCKV, "lockv", 0, 0);
    CHECK(pal_desktop_register(&V[LAUNCH]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[A]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[OV1]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[OV2]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[CTL]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[NOTIF]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[LOCKV]) == NEMA_OK);
    pal_desktop_open("a");
    pal_desktop_open("launcher");
    RUN(push_rejects_null_and_duplicates_and_counts);
    RUN(overlay_stack_is_capped_at_four);
    RUN(status_items_are_limited_to_twelve);
    RUN(task_starts_and_launcher_is_front);
    RUN(overlay_gets_keys_before_the_front_view);
    RUN(escape_pops_an_overlay_that_does_not_take_it);
    RUN(escape_stays_with_an_overlay_that_takes_it);
    RUN(obscured_and_exposed_tell_the_layer_below);
    RUN(opaque_overlay_stops_lower_layers_being_drawn);
    RUN(partial_redraw_draws_only_the_top_overlay);
    RUN(home_keys_open_bound_views_as_opaque_overlays);
    RUN(home_binding_does_not_apply_away_from_home);
    RUN(unknown_bound_view_and_unbinding);
    RUN(switch_key_opens_the_switcher_and_enter_picks);
    RUN(home_key_clears_the_overlays);
    RUN(lock_routes_every_key_to_the_lock_view);
    RUN(lock_needs_a_registered_lock_view);
    RUN(desktop_toast_is_shown_and_removed_by_the_timer);
    RUN(sleep_then_the_first_key_only_wakes_the_screen);
    RUN(link_activity_resets_idle_and_does_not_wake);
    return TEST_DONE();
}
