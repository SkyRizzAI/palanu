/* The rules of ADR-0010 for the real sessions (classic, system/src/ui_wire.c, and Athena, system/src/athena/athena.c) on a running
 * desktop task: what a session registers is its own and leaves with it; leaving any screen with Esc lands on the running session's
 * home; the lock screen is the session's own; Ctrl+Alt+Esc reaches the system console from every screen, the lock included; with no
 * session the console has the screen. Keys go in through pal_input_inject like real presses. */
#include <string.h>

#include "nema/nema.h"
#include "pal/athena.h"
#include "pal/console.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/display.h"
#include "pal/input.h"
#include "pal/dialog.h"
#include "pal/overlay.h"
#include "pal/shell.h"
#include "pal/theme.h"
#include "pal/ui.h"
#include "test.h"

extern const pal_keyboard_t HHV2_KEYBOARD;
size_t desk_session_views(const char *id); /* system/src/desktop_int.h */

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) { (void)ctx, (void)px, (void)stride, (void)r, (void)pal; }
static void present565(void *ctx, const uint16_t *px, int stride, pal_rect_t r) { (void)ctx, (void)px, (void)stride, (void)r; }

/* An app every session can open: it takes no key, so Esc is left to the session. */
static void app_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) { (void)v, (void)c, (void)a, (void)full; }
static pal_view_t APP = {.id = "app", .title = "App", .draw = app_draw};

static void key(uint8_t sym, uint8_t mods) {
    pal_key_t k = {sym, mods, false, 0};
    pal_input_inject(&k);
}

static bool wait_front(const char *id) {
    for (int t = 0; t < 3000; t += 5) {
        if (!strcmp(pal_desktop_front_id(), id)) return true;
        nema_port_sleep_ms(5);
    }
    return false;
}

static bool wait_top(const char *id) {
    for (int t = 0; t < 3000; t += 5) {
        pal_view_t *top = pal_desktop_overlay_top();
        if (id ? top && !strcmp(top->id, id) : top == NULL) return true;
        nema_port_sleep_ms(5);
    }
    return false;
}

static bool wait_console(bool front) {
    for (int t = 0; t < 3000; t += 5) {
        if (pal_console_front() == front) return true;
        nema_port_sleep_ms(5);
    }
    return false;
}

/* Every screen of the session is its own: none of the other session's screens exist while it runs. */
static void only_views_of(const char *running, const char *other) {
    CHECK(desk_session_views(other) == 0);
    CHECK(desk_session_views(running) > 0);
}

static void esc_from_app_lands_home(const char *home) {
    pal_desktop_open("app");
    CHECK(wait_front("app"));
    key(PAL_KEY_ESC, 0);
    CHECK(wait_front(home));
}

TEST(classic_owns_its_screens_and_esc_lands_on_its_home) {
    CHECK(pal_desktops_use("classic") == NEMA_OK);
    CHECK(wait_front("home"));
    only_views_of("classic", "athena");
    CHECK(pal_desktop_view("control") != NULL);
    CHECK(pal_desktop_view("athena") == NULL);
    esc_from_app_lands_home("home");
    key(PAL_KEY_UP, 0); /* the classic home opens its control center */
    CHECK(wait_top("control"));
    key(PAL_KEY_ESC, 0);
    CHECK(wait_top(NULL));
    CHECK(wait_front("home"));
}

TEST(athena_has_none_of_the_classic_screens_and_esc_lands_on_its_home) {
    CHECK(pal_desktops_use("athena") == NEMA_OK);
    CHECK(wait_front("athena"));
    only_views_of("athena", "classic");
    const char *const classic[] = {"home", "lock", "launcher", "applist", "control", "notifications", "splash"};
    for (size_t i = 0; i < sizeof(classic) / sizeof(classic[0]); i++) CHECK(pal_desktop_view(classic[i]) == NULL);
    esc_from_app_lands_home("athena");
}

TEST(the_lock_screen_is_the_session_own_and_the_console_is_reachable_from_it) {
    CHECK(pal_desktop_lock() == NEMA_OK);
    CHECK(wait_top("athena-lock"));
    key(PAL_KEY_ESC, PAL_MOD_CTRL | PAL_MOD_ALT);
    CHECK(wait_console(true));
    key(PAL_KEY_ESC, PAL_MOD_CTRL | PAL_MOD_ALT);
    CHECK(wait_console(false));
    key(PAL_KEY_ENTER, 0);
    key(PAL_KEY_ENTER, 0); /* no PIN: Enter twice */
    CHECK(wait_top(NULL));
    CHECK(!pal_desktop_locked());
}

TEST(switching_back_takes_every_athena_screen_away) {
    for (int i = 0; i < 3; i++) { /* LVGL starts and stops each time */
        CHECK(pal_desktops_use("classic") == NEMA_OK);
        CHECK(wait_front("home"));
        only_views_of("classic", "athena");
        CHECK(pal_desktops_use("athena") == NEMA_OK);
        CHECK(wait_front("athena"));
        only_views_of("athena", "classic");
    }
    CHECK(pal_desktops_use("classic") == NEMA_OK);
    CHECK(pal_desktop_view("athena-lock") == NULL);
}

TEST(with_no_session_the_console_has_the_screen_and_esc_goes_nowhere) {
    pal_desktops_stop();
    CHECK(wait_console(true));
    CHECK(desk_session_views("classic") == 0);
    CHECK(strcmp(pal_desktop_home_id(), "") == 0);
    CHECK(pal_desktop_lock() != NEMA_OK); /* no session, no lock screen */
    key(PAL_KEY_ESC, PAL_MOD_CTRL | PAL_MOD_ALT);
    nema_port_sleep_ms(100);
    CHECK(pal_console_front()); /* nothing to switch to */
    CHECK(pal_desktops_use("classic") == NEMA_OK);
    pal_console_hide();
    CHECK(wait_console(false));
    CHECK(wait_front("home"));
}

static volatile int g_answer = -2;
static void on_answer(bool ok, void *ctx) { (void)ctx, g_answer = ok; }

static bool wait_answer(int want) {
    for (int t = 0; t < 3000; t += 5) {
        if (g_answer == want) return true;
        nema_port_sleep_ms(5);
    }
    return false;
}

/* P3: a service is data the running session draws; its keys and its answer are the system's under every session. */
TEST(a_dialog_is_drawn_by_athena_and_answered_by_the_system) {
    CHECK(pal_desktops_use("athena") == NEMA_OK);
    CHECK(wait_front("athena"));
    g_answer = -2;
    CHECK(pal_dialog_confirm("Erase", "Erase it?", "Erase", "Keep", on_answer, NULL) == NEMA_OK);
    CHECK(wait_top("dialog"));
    pal_shell_view_t v;
    bool described = false;
    for (int t = 0; t < 3000 && !described; t += 5, nema_port_sleep_ms(5)) described = pal_shell_top(&v);
    CHECK(described && v.kind == PAL_SHELL_DIALOG && !strcmp(v.items[1], "Erase"));
    key(PAL_KEY_RIGHT, 0);
    key(PAL_KEY_ENTER, 0);
    CHECK(wait_answer(1));
    CHECK(wait_top(NULL));
}

TEST(with_no_session_the_console_takes_a_dialog_keys) {
    pal_desktops_stop();
    CHECK(wait_console(true));
    g_answer = -2;
    CHECK(pal_dialog_confirm("Restart", "Restart now?", "Restart", "Cancel", on_answer, NULL) == NEMA_OK);
    CHECK(wait_top("dialog"));
    key(PAL_KEY_ESC, 0); /* the console gives the key to the dialog: Esc cancels */
    CHECK(wait_answer(0));
    CHECK(wait_top(NULL));
    CHECK(pal_desktops_use("classic") == NEMA_OK);
    pal_console_hide();
    CHECK(wait_console(false));
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_input_init(&HHV2_KEYBOARD);
    pal_display_ops_t ops = {.width = 320, .height = 240, .present = present, .present_rgb565 = present565};
    pal_display_init(&ops);
    pal_theme_select(0, false, 1);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    pal_console_init(false);
    pal_ui_register_all();
    pal_athena_register();
    pal_desktop_register(&APP);
    pal_desktops_use("classic");
    pal_console_pump(1);
    pal_console_pump(6000); /* the boot is over: the session has the screen (before the task, which owns the console after) */
    pal_desktop_start();
    RUN(classic_owns_its_screens_and_esc_lands_on_its_home);
    RUN(athena_has_none_of_the_classic_screens_and_esc_lands_on_its_home);
    RUN(the_lock_screen_is_the_session_own_and_the_console_is_reachable_from_it);
    RUN(switching_back_takes_every_athena_screen_away);
    RUN(with_no_session_the_console_has_the_screen_and_esc_goes_nowhere);
    RUN(a_dialog_is_drawn_by_athena_and_answered_by_the_system);
    RUN(with_no_session_the_console_takes_a_dialog_keys);
    return TEST_DONE();
}
