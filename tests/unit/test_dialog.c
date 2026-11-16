/* Dialogs, text input, picker and busy overlay (system/src/dialog.c, textinput.c, picker.c, busy.c) on the running desktop: the
 * keys drive them like the physical keyboard, the callbacks carry the answers, and the blocking variants answer a caller on
 * another task. A blocking call made on the desktop task returns false at once instead of stopping the desktop. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/display.h"
#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/overlay.h"
#include "test.h"

extern const pal_keyboard_t HHV2_KEYBOARD;

static volatile int g_presents;
static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    (void)ctx, (void)px, (void)stride, (void)r, (void)pal;
    g_presents++;
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

/* ---- answers, written by the callbacks on the desktop task ---- */
static volatile int confirm_calls;
static volatile bool confirm_ok;
static volatile int input_calls;
static volatile bool input_ok;
static char input_text[PAL_DIALOG_TEXT];
static volatile int picker_calls;
static volatile int picker_index;

static void on_confirm(bool ok, void *ctx) {
    (void)ctx;
    confirm_ok = ok;
    confirm_calls++;
}
static void on_input(bool ok, const char *text, void *ctx) {
    (void)ctx;
    input_ok = ok;
    nema_strlcpy(input_text, text, sizeof(input_text));
    input_calls++;
}
static void on_picker(int index, void *ctx) {
    (void)ctx;
    picker_index = index;
    picker_calls++;
}

static bool top_is(const char *id) {
    pal_view_t *t = pal_desktop_overlay_top();
    return t && strcmp(t->id, id) == 0;
}
static bool wait_top(const char *id) {
    for (int t = 0; t < 2000; t += 5) {
        if (top_is(id)) return true;
        sleep_ms(5);
    }
    return top_is(id);
}
static bool wait_no_overlay(void) {
    for (int t = 0; t < 2000; t += 5) {
        if (pal_desktop_overlay_count() == 0) return true;
        sleep_ms(5);
    }
    return pal_desktop_overlay_count() == 0;
}
static bool wait_int(volatile int *v, int want) {
    for (int t = 0; t < 2000; t += 5) {
        if (*v == want) return true;
        sleep_ms(5);
    }
    return *v == want;
}
static bool wait_flag(volatile bool *v) {
    for (int t = 0; t < 3000; t += 5) {
        if (*v) return true;
        sleep_ms(5);
    }
    return *v;
}
static void settle(void) { sleep_ms(120); }

/* ---- a view that asks a blocking question from the desktop task ---- */
static volatile bool dw_done;
static volatile bool dw_result = true;
static bool dw_key(pal_view_t *v, const pal_key_t *k) {
    (void)v, (void)k;
    dw_result = pal_dialog_confirm_wait("t", "x", NULL, NULL);
    dw_done = true;
    return true;
}
static void dw_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)c, (void)a, (void)full;
}
static pal_view_t DW = {.id = "dw", .title = "dw", .draw = dw_draw, .key = dw_key};

/* ---- a caller on another task ---- */
static volatile bool task_done;
static volatile bool task_result;
static char task_text[64];
static volatile bool task_started;

static void task_confirm(void *arg) {
    (void)arg;
    task_started = true;
    task_result = pal_dialog_confirm_wait("Other task", "Proceed?", "Yes", "No");
    task_done = true;
}
static void task_input(void *arg) {
    (void)arg;
    task_started = true;
    task_result = pal_textinput_wait("Name", "", 16, false, task_text, sizeof(task_text));
    task_done = true;
}
static void start_task(void (*fn)(void *)) {
    task_done = false;
    task_started = false;
    nema_task_desc_t td = {.name = "waiter", .fn = fn, .arg = NULL, .stack = 6144, .prio = 5, .core = -1,
                           .role = NEMA_TASK_SYSTEM, .owner = NEMA_OWNER_KERNEL};
    CHECK(nema_task_create(&td) == NEMA_OK);
}

/* ---------------------------------------------------------------- tests */
TEST(confirm_ok_with_enter) {
    int c0 = confirm_calls;
    CHECK(pal_dialog_confirm("Erase", "Are you sure?", "Erase", "Keep", on_confirm, NULL) == NEMA_OK);
    CHECK(wait_top("dialog"));
    settle();
    key(PAL_KEY_RIGHT); /* the focus starts on Cancel (the left button), so OK is one step right */
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&confirm_calls, c0 + 1));
    CHECK(confirm_ok);
    CHECK(wait_no_overlay());
}

TEST(confirm_cancel_with_escape) {
    int c0 = confirm_calls;
    CHECK(pal_dialog_confirm("Erase", "Are you sure?", NULL, NULL, on_confirm, NULL) == NEMA_OK);
    CHECK(wait_top("dialog"));
    key(PAL_KEY_ESC);
    CHECK(wait_int(&confirm_calls, c0 + 1));
    CHECK(!confirm_ok);
    CHECK(wait_no_overlay());
}

TEST(confirm_arrows_choose_the_button) {
    int c0 = confirm_calls;
    CHECK(pal_dialog_confirm("Erase", "Are you sure?", "Erase", "Keep", on_confirm, NULL) == NEMA_OK);
    CHECK(wait_top("dialog"));
    key(PAL_KEY_RIGHT); /* to Erase, the right button */
    key(PAL_KEY_LEFT);  /* back to Keep, the left button */
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&confirm_calls, c0 + 1));
    CHECK(!confirm_ok);
    CHECK(wait_no_overlay());
}

TEST(confirm_is_cancelled_when_the_desktop_goes_home) {
    int c0 = confirm_calls;
    CHECK(pal_dialog_confirm("Erase", "Are you sure?", NULL, NULL, on_confirm, NULL) == NEMA_OK);
    CHECK(wait_top("dialog"));
    key(PAL_KEY_GOHOME);
    CHECK(wait_int(&confirm_calls, c0 + 1));
    CHECK(!confirm_ok);
    CHECK(wait_no_overlay());
}

TEST(two_dialogs_then_the_pool_is_full) {
    int c0 = confirm_calls;
    CHECK(pal_dialog_confirm("one", "1", NULL, NULL, on_confirm, NULL) == NEMA_OK);
    CHECK(pal_dialog_confirm("two", "2", NULL, NULL, on_confirm, NULL) == NEMA_OK);
    CHECK(pal_dialog_confirm("three", "3", NULL, NULL, on_confirm, NULL) == NEMA_ERR_FULL);
    CHECK(wait_top("dialog"));
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&confirm_calls, c0 + 1));
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&confirm_calls, c0 + 2));
    CHECK(wait_no_overlay());
}

TEST(text_input_editing_sequence) {
    int c0 = input_calls;
    CHECK(pal_textinput_open("Edit", "ab", 6, false, on_input, NULL) == NEMA_OK);
    CHECK(wait_top("textinput"));
    key_char('c');     /* abc */
    key(PAL_KEY_LEFT); /* caret before c */
    key_char('X');     /* abXc */
    key(PAL_KEY_BACKSPACE); /* abc */
    key(PAL_KEY_DELETE);    /* caret after b? the caret is at 2: deletes c -> ab */
    key(PAL_KEY_HOME);
    key(PAL_KEY_DELETE); /* deletes a -> b */
    key(PAL_KEY_END);
    key_char('z'); /* bz */
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&input_calls, c0 + 1));
    CHECK(input_ok);
    CHECK(strcmp(input_text, "bz") == 0);
    CHECK(wait_no_overlay());
}

TEST(text_input_stops_at_max_length_and_escape_cancels) {
    int c0 = input_calls;
    CHECK(pal_textinput_open("Short", "", 3, false, on_input, NULL) == NEMA_OK);
    CHECK(wait_top("textinput"));
    key_char('a');
    key_char('b');
    key_char('c');
    key_char('d'); /* refused: the field holds three */
    settle();
    key(PAL_KEY_ESC);
    CHECK(wait_int(&input_calls, c0 + 1));
    CHECK(!input_ok);
    CHECK(strcmp(input_text, "") == 0);
    CHECK(wait_no_overlay());
}

TEST(masked_text_input_returns_the_real_text) {
    int c0 = input_calls;
    CHECK(pal_textinput_open("Password", "", 16, true, on_input, NULL) == NEMA_OK);
    CHECK(wait_top("textinput"));
    key_char('p');
    key_char('w');
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&input_calls, c0 + 1));
    CHECK(input_ok);
    CHECK(strcmp(input_text, "pw") == 0);
    CHECK(wait_no_overlay());
}

TEST(picker_moves_with_wrap_and_picks) {
    static const char *const items[3] = {"one", "two", "three"};
    int c0 = picker_calls;
    CHECK(pal_picker_open("Pick", items, 3, on_picker, NULL) == NEMA_OK);
    CHECK(wait_top("picker"));
    key(PAL_KEY_DOWN);
    key(PAL_KEY_DOWN);
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&picker_calls, c0 + 1));
    CHECK_EQ(picker_index, 2);
    CHECK(wait_no_overlay());

    CHECK(pal_picker_open("Pick", items, 3, on_picker, NULL) == NEMA_OK);
    CHECK(wait_top("picker"));
    key(PAL_KEY_UP); /* wraps from the first to the last */
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_int(&picker_calls, c0 + 2));
    CHECK_EQ(picker_index, 2);
    CHECK(wait_no_overlay());

    CHECK(pal_picker_open("Pick", items, 3, on_picker, NULL) == NEMA_OK);
    CHECK(wait_top("picker"));
    key(PAL_KEY_ESC);
    CHECK(wait_int(&picker_calls, c0 + 3));
    CHECK_EQ(picker_index, -1);
    CHECK(pal_picker_open("Pick", items, PAL_PICKER_ITEMS + 1, on_picker, NULL) == NEMA_ERR_INVAL);
    CHECK(wait_no_overlay());
}

TEST(busy_overlay_takes_every_key_until_hidden) {
    pal_busy_show("Connecting");
    CHECK(wait_top("busy"));
    CHECK(pal_busy_showing());
    key(PAL_KEY_ESC); /* the user cannot back out */
    key_char('q');
    settle();
    CHECK(top_is("busy"));
    pal_busy_show("Still connecting"); /* showing again only changes the text */
    settle();
    CHECK_EQ(pal_desktop_overlay_count(), 1);
    pal_busy_hide();
    CHECK(wait_no_overlay());
    CHECK(!pal_busy_showing());
}

TEST(blocking_confirm_answers_a_caller_on_another_task) {
    start_task(task_confirm);
    CHECK(wait_top("dialog"));
    settle();
    key(PAL_KEY_RIGHT); /* "Yes" is the right button; the focus starts on "No" */
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_flag(&task_done));
    CHECK(task_result);
    CHECK(wait_no_overlay());
}

TEST(blocking_text_input_answers_a_caller_on_another_task) {
    start_task(task_input);
    CHECK(wait_top("textinput"));
    settle();
    key_char('h');
    key_char('i');
    settle();
    key(PAL_KEY_ENTER);
    CHECK(wait_flag(&task_done));
    CHECK(task_result);
    CHECK(strcmp(task_text, "hi") == 0);
    CHECK(wait_no_overlay());
}

TEST(blocking_call_on_the_desktop_task_returns_false_at_once) {
    CHECK(pal_desktop_register(&DW) == NEMA_OK);
    pal_desktop_open("dw");
    for (int t = 0; t < 2000 && strcmp(pal_desktop_front_id(), "dw") != 0; t += 5) sleep_ms(5);
    CHECK(strcmp(pal_desktop_front_id(), "dw") == 0);
    key_char('x');
    CHECK(wait_flag(&dw_done)); /* it returned: the desktop did not stop */
    CHECK(!dw_result);
    CHECK(pal_desktop_overlay_count() == 0);
    pal_desktop_home();
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_input_init(&HHV2_KEYBOARD);
    pal_display_ops_t ops = {.width = 320, .height = 240, .present = present, .backlight = NULL, .ctx = NULL};
    pal_display_init(&ops);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    static pal_view_t launcher = {.id = "launcher", .title = "Home", .flags = PAL_VIEW_HIDDEN, .draw = dw_draw};
    CHECK(pal_desktop_register(&launcher) == NEMA_OK);
    CHECK(pal_desktop_start() == NEMA_OK);
    for (int t = 0; t < 2000 && strcmp(pal_desktop_front_id(), "launcher") != 0; t += 5) sleep_ms(5);
    RUN(confirm_ok_with_enter);
    RUN(confirm_cancel_with_escape);
    RUN(confirm_arrows_choose_the_button);
    RUN(confirm_is_cancelled_when_the_desktop_goes_home);
    RUN(two_dialogs_then_the_pool_is_full);
    RUN(text_input_editing_sequence);
    RUN(text_input_stops_at_max_length_and_escape_cancels);
    RUN(masked_text_input_returns_the_real_text);
    RUN(picker_moves_with_wrap_and_picks);
    RUN(busy_overlay_takes_every_key_until_hidden);
    RUN(blocking_confirm_answers_a_caller_on_another_task);
    RUN(blocking_text_input_answers_a_caller_on_another_task);
    RUN(blocking_call_on_the_desktop_task_returns_false_at_once);
    return TEST_DONE();
}
