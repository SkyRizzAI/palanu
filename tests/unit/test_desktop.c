/* The desktop compositor (system/src/desktop.c) with fake views that count what happens to them.
 * Registration, open and front, and the MRU order behind the switch key are checked before the desktop task
 * starts (the main thread is then the only writer). After the start, the switch, home, and escape keys are
 * injected like real key presses and the task's effects are polled for. The display is a 64x48 framebuffer
 * whose present callback only counts; no panel is needed. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/kui.h"
#include "pal/overlay.h"
#include "test.h"

extern const pal_keyboard_t HHV2_KEYBOARD;

typedef struct {
    volatile int enter, leave, draw, draw_full, key, pump, tick;
    volatile bool last_full, handle;
} fake_t;

#define NVIEWS 22
static fake_t F[NVIEWS];
static pal_view_t V[NVIEWS];
static char IDS[NVIEWS][16];
static volatile int g_presents;

static void v_enter(pal_view_t *v) { ((fake_t *)v->data)->enter++; }
static void v_leave(pal_view_t *v) { ((fake_t *)v->data)->leave++; }
static void v_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    fake_t *f = (fake_t *)v->data;
    pal_fill(c, a.x, a.y, 1, 1, full ? 2 : 3); /* one pixel, so a redraw has something to present */
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

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    (void)ctx, (void)px, (void)stride, (void)r, (void)pal;
    g_presents++;
}

static void make_view(int i, const char *id, uint32_t tick_ms) {
    nema_strlcpy(IDS[i], id, sizeof(IDS[i]));
    V[i].id = IDS[i];
    V[i].title = IDS[i];
    V[i].flags = 0;
    V[i].enter = v_enter;
    V[i].leave = v_leave;
    V[i].draw = v_draw;
    V[i].key = v_key;
    V[i].pump = v_pump;
    V[i].tick = tick_ms ? v_tick : NULL;
    V[i].tick_ms = tick_ms;
    V[i].data = &F[i];
}

/* view indices */
enum { LAUNCH, A, B, C, EXTRA0 = 4, AREA = 21 };

static void key(uint8_t sym) {
    pal_key_t k = {sym, 0, false, 0};
    pal_input_inject(&k);
}
static bool wait_front(const char *id) {
    for (int t = 0; t < 2000; t += 5) {
        if (strcmp(pal_desktop_front_id(), id) == 0) return true;
        nema_port_sleep_ms(5);
    }
    return strcmp(pal_desktop_front_id(), id) == 0;
}
/* Wait until *counter is greater than above, for up to ms. */
static bool wait_above(volatile int *counter, int above, int ms) {
    for (int t = 0; t < ms; t += 5) {
        if (*counter > above) return true;
        nema_port_sleep_ms(5);
    }
    return *counter > above;
}
static void settle(void) { nema_port_sleep_ms(120); }
/* The cycle key opens the switcher; Enter picks the app it stands on. */
static bool wait_switcher(void) {
    for (int t = 0; t < 2000; t += 5) {
        pal_view_t *top = pal_desktop_overlay_top();
        if (top && strcmp(top->id, "switcher") == 0) return true;
        nema_port_sleep_ms(5);
    }
    return false;
}
static void cycle_to(const char *id) {
    key(PAL_KEY_SWITCH);
    CHECK(wait_switcher());
    key(PAL_KEY_ENTER);
    CHECK(wait_front(id));
}

/* ---------------------------------------------------------------- before the task runs */
TEST(register_rejects_duplicates_and_nulls) {
    CHECK(pal_desktop_register(NULL) == NEMA_ERR_INVAL);
    CHECK(pal_desktop_register(&V[A]) == NEMA_ERR_EXIST); /* same id, already registered in main */
    CHECK(pal_desktop_view("a") == &V[A]);
    CHECK(pal_desktop_view("nope") == NULL);
    CHECK_EQ(pal_desktop_view_count(), 4);
    CHECK(pal_desktop_view_at(0) == &V[LAUNCH]);
    CHECK(pal_desktop_view_at(4) == NULL);
}

TEST(register_has_a_limit_of_twenty) {
    for (int i = EXTRA0; i < 20; i++) {
        make_view(i, "x", 0);
        IDS[i][1] = (char)('0' + (i - EXTRA0) / 10);
        IDS[i][2] = (char)('0' + (i - EXTRA0) % 10);
        IDS[i][3] = 0;
        CHECK(pal_desktop_register(&V[i]) == NEMA_OK);
    }
    CHECK_EQ(pal_desktop_view_count(), 20);
    make_view(20, "over", 0);
    CHECK(pal_desktop_register(&V[20]) == NEMA_ERR_FULL);
    CHECK_EQ(pal_desktop_view_count(), 20);
}

TEST(open_makes_the_view_front_and_calls_enter_leave) {
    CHECK(strcmp(pal_desktop_front_id(), "") == 0);
    pal_desktop_open("a");
    CHECK(strcmp(pal_desktop_front_id(), "a") == 0);
    CHECK_EQ(F[A].enter, 1);
    pal_desktop_open("a"); /* already in front: nothing happens */
    CHECK_EQ(F[A].enter, 1);
    pal_desktop_open("missing");
    CHECK(strcmp(pal_desktop_front_id(), "a") == 0);
    pal_desktop_open("b");
    CHECK(strcmp(pal_desktop_front_id(), "b") == 0);
    CHECK_EQ(F[A].leave, 1);
    CHECK_EQ(F[B].enter, 1);
    CHECK_EQ(F[B].leave, 0);
}

TEST(area_excludes_status_bar_unless_fullscreen) {
    pal_canvas_t *c = pal_display_canvas();
    make_view(AREA, "area", 0); /* not registered: the area does not depend on registration */
    pal_rect_t r = pal_desktop_area(&V[A]);
    CHECK_EQ(r.x, 0);
    CHECK(r.y > 0);
    CHECK_EQ(r.w, c->w);
    CHECK_EQ(r.y + r.h, c->h);
    V[AREA].flags = PAL_VIEW_FULLSCREEN;
    pal_rect_t f = pal_desktop_area(&V[AREA]);
    CHECK_EQ(f.y, 0);
    CHECK_EQ(f.h, c->h);
}

/* ---------------------------------------------------------------- with the task running */
TEST(task_starts_once_and_activates_launcher) {
    CHECK(pal_desktop_start() == NEMA_OK);
    CHECK(pal_desktop_start() == NEMA_ERR_BUSY);
    CHECK(wait_front("launcher"));
    CHECK(F[LAUNCH].enter >= 1);
}

TEST(tick_runs_only_for_the_front_view) {
    CHECK(wait_front("launcher"));
    nema_port_sleep_ms(400);
    CHECK(F[LAUNCH].tick >= 3); /* launcher ticks every 50 ms */
    CHECK_EQ(F[A].tick, 0);     /* a has a tick too, but it is not in front */
}

TEST(switch_key_opens_the_switcher_on_the_previous_app) {
    /* open order before the start was a, b, c, a: the MRU is a, c, b; the task then put launcher first */
    CHECK(wait_front("launcher"));
    key(PAL_KEY_SWITCH);
    CHECK(wait_switcher()); /* the switcher starts on the app before the front one */
    key(PAL_KEY_ENTER);
    CHECK(wait_front("a"));
    cycle_to("launcher");
    cycle_to("a");
}

TEST(home_key_goes_to_launcher) {
    key(PAL_KEY_GOHOME);
    CHECK(wait_front("launcher"));
}

TEST(escape_goes_home_unless_the_view_handles_it) {
    CHECK(wait_front("launcher"));
    cycle_to("a");
    F[A].handle = true;
    key(PAL_KEY_ESC);
    nema_port_sleep_ms(100);
    CHECK(strcmp(pal_desktop_front_id(), "a") == 0); /* handled by the view */
    F[A].handle = false;
    key(PAL_KEY_ESC);
    CHECK(wait_front("launcher"));
}

TEST(keys_go_to_the_front_view) {
    CHECK(wait_front("launcher"));
    int before_launch = F[LAUNCH].key, before_a = F[A].key;
    key(PAL_KEY_CHAR);
    nema_port_sleep_ms(100);
    CHECK(F[LAUNCH].key > before_launch);
    CHECK_EQ(F[A].key, before_a);
}

TEST(switching_redraws_the_new_front_in_full) {
    CHECK(wait_front("launcher"));
    settle();
    int full_before = F[A].draw_full, enter_before = F[A].enter;
    key(PAL_KEY_SWITCH);
    CHECK(wait_switcher());
    key(PAL_KEY_ENTER);
    CHECK(wait_front("a"));
    CHECK(wait_above(&F[A].draw_full, full_before, 2000));
    CHECK(F[A].last_full);
    CHECK_EQ(F[A].enter, enter_before + 1);
}

TEST(invalidate_redraws_front_without_full_flag) {
    CHECK(wait_front("a"));
    settle();
    int d0 = F[A].draw, full0 = F[A].draw_full, p0 = g_presents;
    pal_desktop_invalidate();
    CHECK(wait_above(&F[A].draw, d0, 2000));
    CHECK(!F[A].last_full);
    CHECK_EQ(F[A].draw_full, full0);
    CHECK(g_presents > p0); /* the changed rectangle went to the panel */
}

TEST(invalidate_all_redraws_everything) {
    CHECK(wait_front("a"));
    settle();
    int full0 = F[A].draw_full, p0 = g_presents;
    pal_desktop_invalidate_all();
    CHECK(wait_above(&F[A].draw_full, full0, 2000));
    CHECK(F[A].last_full);
    CHECK(g_presents > p0);
}

TEST(no_redraw_when_nothing_is_invalid) {
    CHECK(wait_front("a"));
    settle();
    int d0 = F[A].draw;
    nema_port_sleep_ms(1200); /* the loop wakes once a second for the clock; that alone must not draw */
    CHECK_EQ(F[A].draw, d0);
}

TEST(pump_runs_for_every_view) {
    settle();
    CHECK(F[LAUNCH].pump > 0);
    CHECK(F[A].pump > 0);
    CHECK(F[B].pump > 0); /* background views are pumped too */
}

/* A status item (Bluetooth, the card, the unread dot) is drawn from the clock's right edge, not over its last digit: at 2x the
 * clock ends at pixel 38, and the items once started at 34, which smeared the minutes digit on a board with Bluetooth on. */
static volatile int g_item_x = -1, g_item_calls;
static int item_draw(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st) {
    (void)c, (void)t, (void)h, (void)st;
    g_item_x = x;
    g_item_calls++;
    return 6;
}

TEST(status_items_start_right_of_the_clock) {
    static const pal_status_item_t item = {.id = "test.item", .draw = item_draw};
    CHECK(pal_status_item_register(&item) == NEMA_OK);
    int n0 = g_item_calls;
    pal_desktop_invalidate_all();
    CHECK(wait_above((volatile int *)&g_item_calls, n0, 2000));
    CHECK(g_item_x >= kui_top_px(19)); /* the clock's right edge at the current UI scale (kui_status.c) */
    CHECK_EQ(g_item_x, kui_top_px(KUI_STATUS_ITEMS_X));
}

/* A session's part of the compositor (ADR-0010): the home, and the cycle key opening the switcher, as the classic session sets them. */
nema_err_t pal_switcher_open(void);
size_t pal_desktop_overlay_count(void);
static bool session_key(const pal_key_t *k) {
    if (k->sym == PAL_KEY_SWITCH && pal_desktop_overlay_count() == 0) return pal_switcher_open(), true;
    return false;
}
static const pal_desktop_chrome_t SESSION = {.key = session_key};

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_input_init(&HHV2_KEYBOARD);
    pal_display_ops_t ops = {.width = 64, .height = 48, .present = present, .backlight = NULL, .ctx = NULL};
    pal_display_init(&ops);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    pal_desktop_set_home("launcher");
    pal_desktop_set_chrome(&SESSION);
    make_view(LAUNCH, "launcher", 50);
    make_view(A, "a", 50);
    make_view(B, "b", 0);
    make_view(C, "c", 0);
    CHECK(pal_desktop_register(&V[LAUNCH]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[A]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[B]) == NEMA_OK);
    CHECK(pal_desktop_register(&V[C]) == NEMA_OK);
    RUN(register_rejects_duplicates_and_nulls);
    RUN(register_has_a_limit_of_twenty);
    RUN(open_makes_the_view_front_and_calls_enter_leave);
    RUN(area_excludes_status_bar_unless_fullscreen);
    /* MRU before the start: open a, b, c, a (the open_ test already opened a and b, so this is a, b, c, a) */
    pal_desktop_open("c");
    pal_desktop_open("a");
    RUN(task_starts_once_and_activates_launcher);
    RUN(tick_runs_only_for_the_front_view);
    RUN(switch_key_opens_the_switcher_on_the_previous_app);
    RUN(home_key_goes_to_launcher);
    RUN(escape_goes_home_unless_the_view_handles_it);
    RUN(keys_go_to_the_front_view);
    RUN(switching_redraws_the_new_front_in_full);
    RUN(invalidate_redraws_front_without_full_flag);
    RUN(invalidate_all_redraws_everything);
    RUN(no_redraw_when_nothing_is_invalid);
    RUN(pump_runs_for_every_view);
    RUN(status_items_start_right_of_the_clock);
    return TEST_DONE();
}
