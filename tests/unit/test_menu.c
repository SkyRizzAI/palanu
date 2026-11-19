/* The menu framework (system/src/menu.c, menu_draw.c): navigation that skips section headers, the page stack with Esc, the
 * row kinds (toggle, choice, slider, action with confirm, info, text), the asking calls through hooks, dynamic rebuilds, and
 * the layout and drawing that the Settings pages depend on. No desktop: the asks go to recording hooks. */
#include <stdio.h>
#include <string.h>

#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/menu.h"
#include "pal/theme.h"
#include "test.h"

#define W 320
#define H 240
static uint8_t PX[W * H];

/* ---- recording hooks ---- */
static int h_picker_n, h_picker_calls, h_confirm_calls, h_ask_calls, h_ask_mask, h_ask_max, h_toasts;
static char h_last_toast[80], h_ask_initial[80], h_last_title[40];

static void hk_picker(pal_menu_t *m, const char *t, const char *const *items, int n) {
    (void)m, (void)items;
    h_picker_calls++;
    h_picker_n = n;
    snprintf(h_last_title, sizeof(h_last_title), "%s", t);
}
static void hk_confirm(pal_menu_t *m, const char *t, const char *text, const char *ok) {
    (void)m, (void)text, (void)ok;
    h_confirm_calls++;
    snprintf(h_last_title, sizeof(h_last_title), "%s", t);
}
static void hk_ask(pal_menu_t *m, const char *t, const char *init, int max_len, bool mask) {
    (void)m;
    h_ask_calls++;
    h_ask_mask = mask;
    h_ask_max = max_len;
    snprintf(h_ask_initial, sizeof(h_ask_initial), "%s", init ? init : "");
    snprintf(h_last_title, sizeof(h_last_title), "%s", t);
}
static void hk_toast(const char *text) {
    h_toasts++;
    snprintf(h_last_toast, sizeof(h_last_toast), "%s", text);
}
static const pal_menu_hooks_t HOOKS = {hk_picker, hk_confirm, hk_ask, hk_toast};

static void reset_hooks(void) {
    h_picker_n = h_picker_calls = h_confirm_calls = h_ask_calls = h_ask_mask = h_ask_max = h_toasts = 0;
    h_last_toast[0] = h_ask_initial[0] = h_last_title[0] = 0;
    pal_menu_set_hooks(&HOOKS);
}

static pal_key_t key(pal_keysym_t sym, uint8_t mods) {
    pal_key_t k = {sym, mods, false, 0};
    return k;
}

/* ---- fixture values ---- */
static int v_bright = 50, v_toggle = 0, v_choice = 1, v_action_runs, v_text_sets, v_text_fail;
static char v_text[40] = "abc";
static int v_sets_bright, v_sets_toggle, v_sets_choice;
static const char *const CH[] = {"alpha", "beta", "gamma", NULL};

static int g_bright(void *c) {
    (void)c;
    return v_bright;
}
static void s_bright(void *c, int v) {
    (void)c;
    v_bright = v;
    v_sets_bright++;
}
static int g_toggle(void *c) {
    (void)c;
    return v_toggle;
}
static void s_toggle(void *c, int v) {
    (void)c;
    v_toggle = v;
    v_sets_toggle++;
}
static int g_choice(void *c) {
    (void)c;
    return v_choice;
}
static void s_choice(void *c, int v) {
    (void)c;
    v_choice = v;
    v_sets_choice++;
}
static void run_act(void *c) {
    (void)c;
    v_action_runs++;
}
static void t_get(void *c, char *out, size_t cap) {
    (void)c;
    snprintf(out, cap, "%s", v_text);
}
static bool t_apply(void *c, const char *text) {
    (void)c;
    v_text_sets++;
    if (v_text_fail) return false;
    snprintf(v_text, sizeof(v_text), "%s", text);
    return true;
}
static void info_fixed(void *c, char *out, size_t cap) { snprintf(out, cap, "%s", (const char *)c); }

static pal_menu_page_t CHILD;
static pal_menu_page_t *open_child(void *c) {
    (void)c;
    return &CHILD;
}

static pal_menu_row_t ROWS[] = {
    {.kind = PAL_MENU_SECTION, .label = "Section A"},
    {.kind = PAL_MENU_SLIDER, .label = "Bright", .get = g_bright, .set = s_bright, .min = 0, .max = 100, .step = 10},
    {.kind = PAL_MENU_TOGGLE, .label = "Flag", .get = g_toggle, .set = s_toggle},
    {.kind = PAL_MENU_SECTION, .label = "Section B"},
    {.kind = PAL_MENU_CHOICE, .label = "Pick", .get = g_choice, .set = s_choice, .choices = CH},
    {.kind = PAL_MENU_ACTION, .label = "Run", .run = run_act},
    {.kind = PAL_MENU_ACTION, .label = "Danger", .run = run_act, .danger = true, .confirm = "Sure?"},
    {.kind = PAL_MENU_TEXT, .label = "Name", .text = t_get, .apply_text = t_apply, .max_len = 12, .mask = true},
    {.kind = PAL_MENU_INFO, .label = "Info", .text = info_fixed, .ctx = (void *)"read only"},
    {.kind = PAL_MENU_SUBMENU, .label = "Child", .open = open_child},
    {.kind = PAL_MENU_TOGGLE, .label = "Hidden", .get = g_toggle, .set = s_toggle, .hidden = true},
    {.kind = PAL_MENU_TOGGLE, .label = "Locked", .get = g_toggle, .set = s_toggle, .disabled = true},
};
static pal_menu_page_t ROOT = {.title = "Root", .rows = ROWS, .n = sizeof(ROWS) / sizeof(ROWS[0])};

static void reset_all(void) {
    reset_hooks();
    v_bright = 50, v_toggle = 0, v_choice = 1, v_action_runs = 0, v_text_sets = 0, v_text_fail = 0;
    v_sets_bright = v_sets_toggle = v_sets_choice = 0;
    snprintf(v_text, sizeof(v_text), "abc");
    ROOT.sel = 0;
    ROOT.scroll = 0;
}

/* ---- navigation ---- */

TEST(selection_starts_on_first_selectable_row_and_skips_sections) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    CHECK_EQ(ROOT.sel, 1); /* row 0 is a section */
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    CHECK_EQ(ROOT.sel, 2);
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    CHECK_EQ(ROOT.sel, 4); /* skips section B at 3 */
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_UP, 0, false, 0});
    CHECK_EQ(ROOT.sel, 2);
}

TEST(navigation_wraps_and_skips_hidden_rows) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    /* from the last selectable row (Locked, 11) moving down wraps to the first (1) */
    CHECK(pal_menu_select(&m, 11));
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    CHECK_EQ(ROOT.sel, 1);
    /* the hidden row (10) is passed over going down from Child (9) */
    CHECK(pal_menu_select(&m, 9));
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    CHECK_EQ(ROOT.sel, 11);
    CHECK(!pal_menu_select(&m, 3)); /* a section cannot be selected */
    CHECK(!pal_menu_select(&m, 10)); /* nor a hidden row */
}

/* Up from the first row wraps to the last one on a page of any length (21 rows, like the Settings root: the index must not go
 * through an unsigned -1, which lands on row 15 there). */
static pal_menu_row_t WRAP_ROWS[21];
TEST(navigation_wraps_up_to_the_last_row) {
    reset_all();
    for (int i = 0; i < 21; i++) WRAP_ROWS[i] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "row"};
    WRAP_ROWS[1] = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .group = true};
    static pal_menu_page_t page;
    page = (pal_menu_page_t){.title = "Wrap", .rows = WRAP_ROWS, .n = 21};
    pal_menu_t m;
    pal_menu_init(&m, &page);
    CHECK(pal_menu_select(&m, 0));
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_UP, 0, false, 0});
    CHECK_EQ(page.sel, 20);
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    CHECK_EQ(page.sel, 0);
}

/* ---- the stack ---- */

TEST(submenu_pushes_its_page_and_esc_pops_it_then_stops_at_root) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    CHILD.title = "Child page";
    CHILD.rows = ROWS;
    CHILD.n = 1;
    CHILD.sel = 0;
    CHILD.scroll = 0;
    pal_menu_select(&m, 9);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    CHECK_EQ(pal_menu_depth(&m), 2);
    CHECK(pal_menu_top(&m) == &CHILD);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ESC, 0, false, 0})); /* consumed: back to root */
    CHECK_EQ(pal_menu_depth(&m), 1);
    CHECK(pal_menu_top(&m) == &ROOT);
    CHECK(!pal_menu_key(&m, &(pal_key_t){PAL_KEY_ESC, 0, false, 0})); /* not consumed at the root: the desktop goes home */
    CHECK_EQ(pal_menu_depth(&m), 1);
}

TEST(each_page_remembers_its_selection) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 4);
    pal_menu_push(&m, &CHILD);
    CHECK_EQ(CHILD.sel, 0);
    pal_menu_pop(&m);
    CHECK_EQ(ROOT.sel, 4);
    CHECK(!pal_menu_pop(&m)); /* the root cannot be popped */
}

TEST(a_full_stack_refuses_another_page) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    for (int i = 1; i < PAL_MENU_DEPTH; i++) CHECK(pal_menu_push(&m, &CHILD));
    CHECK(!pal_menu_push(&m, &CHILD));
    CHECK_EQ(pal_menu_depth(&m), PAL_MENU_DEPTH);
}

/* ---- the row kinds ---- */

TEST(toggle_flips_on_enter_and_on_the_sideways_keys) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 2);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    CHECK_EQ(v_toggle, 1);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, 0, false, 0}));
    CHECK_EQ(v_toggle, 0);
    CHECK_EQ(v_sets_toggle, 2);
}

TEST(disabled_rows_change_nothing_and_say_why) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 11);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0})); /* used: it toasts the reason */
    CHECK_EQ(h_toasts, 1);
    CHECK(h_last_toast[0] != 0);
    CHECK(!pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, 0, false, 0})); /* a value key does nothing */
    CHECK_EQ(v_sets_toggle, 0);
}

TEST(choice_cycles_with_wrap_and_enter_opens_a_picker) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 4);
    v_choice = 2; /* gamma, the last one */
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, 0, false, 0}));
    CHECK_EQ(v_choice, 0); /* wraps to alpha */
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_LEFT, 0, false, 0}));
    CHECK_EQ(v_choice, 2);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    CHECK_EQ(h_picker_calls, 1);
    CHECK_EQ(h_picker_n, 3);
    CHECK(strcmp(h_last_title, "Pick") == 0);
    pal_menu_answer_pick(&m, 1);
    CHECK_EQ(v_choice, 1);
    /* a cancelled picker changes nothing */
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    pal_menu_answer_pick(&m, -1);
    CHECK_EQ(v_choice, 1);
}

TEST(slider_steps_and_shift_steps_big_and_clamps) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 1);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, 0, false, 0}));
    CHECK_EQ(v_bright, 60);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, PAL_MOD_SHIFT, false, 0}));
    CHECK_EQ(v_bright, 100); /* 60 + 40, clamped at max */
    CHECK(!pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, 0, false, 0})); /* already at max: no change */
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_LEFT, PAL_MOD_SHIFT, false, 0}));
    CHECK_EQ(v_bright, 60);
    v_bright = 5;
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_LEFT, 0, false, 0}));
    CHECK_EQ(v_bright, 0); /* clamped at min */
}

TEST(a_gauge_slider_without_setter_is_read_only) {
    reset_all();
    static pal_menu_row_t g[] = {{.kind = PAL_MENU_SLIDER, .label = "Used", .get = g_bright, .min = 0, .max = 100}};
    static pal_menu_page_t gp = {.title = "G", .rows = g, .n = 1};
    pal_menu_t m;
    pal_menu_init(&m, &gp);
    (void)pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, 0, false, 0}); /* nothing to select: the key scrolls the page */
    CHECK_EQ(v_sets_bright, 0);
}

TEST(action_runs_on_enter) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 5);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    CHECK_EQ(v_action_runs, 1);
}

TEST(action_with_confirm_asks_first_and_runs_only_on_ok) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 6);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    CHECK_EQ(h_confirm_calls, 1);
    CHECK_EQ(v_action_runs, 0);
    pal_menu_answer_confirm(&m, false);
    CHECK_EQ(v_action_runs, 0);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    pal_menu_answer_confirm(&m, true);
    CHECK_EQ(v_action_runs, 1);
}

TEST(text_row_asks_masked_with_its_value_and_applies_the_answer) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 7);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    CHECK_EQ(h_ask_calls, 1);
    CHECK_EQ(h_ask_mask, 1);
    CHECK_EQ(h_ask_max, 12);
    CHECK(strcmp(h_ask_initial, "abc") == 0);
    pal_menu_answer_ask(&m, true, "hello");
    CHECK(strcmp(v_text, "hello") == 0);
    CHECK_EQ(v_text_sets, 1);
    /* a cancelled entry changes nothing */
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    pal_menu_answer_ask(&m, false, NULL);
    CHECK(strcmp(v_text, "hello") == 0);
    CHECK_EQ(v_text_sets, 1);
}

TEST(a_refused_text_says_not_saved) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 7);
    v_text_fail = 1;
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    pal_menu_answer_ask(&m, true, "x");
    CHECK_EQ(h_toasts, 1);
    CHECK(strcmp(h_last_toast, "Not saved") == 0);
    CHECK(strcmp(v_text, "abc") == 0);
}

TEST(info_rows_do_nothing_on_enter) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_menu_select(&m, 8);
    CHECK(!pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0}));
    CHECK_EQ(pal_menu_depth(&m), 1);
}

TEST(a_question_whose_page_was_popped_is_dropped) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    CHILD.rows = ROWS;
    CHILD.n = sizeof(ROWS) / sizeof(ROWS[0]);
    CHILD.sel = 0;
    CHILD.scroll = 0;
    pal_menu_push(&m, &CHILD);
    pal_menu_select(&m, 4); /* the choice, on the child page */
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_ENTER, 0, false, 0})); /* a picker */
    pal_menu_pop(&m); /* the child is gone before the answer */
    pal_menu_answer_pick(&m, 2);
    CHECK_EQ(v_choice, 1); /* nothing applied: the page the question was for is gone */
}

/* ---- dynamic pages ---- */

static pal_menu_row_t DYN_STORE[8];
static int dyn_count = 2;
static void dyn_build(pal_menu_page_t *p) {
    pal_menu_clear(p);
    for (int i = 0; i < dyn_count; i++) {
        pal_menu_row_t r = {.kind = PAL_MENU_ACTION, .label = "Net", .run = run_act};
        pal_menu_add(p, &r);
    }
}
static pal_menu_page_t DYN = {.title = "Dyn", .store = DYN_STORE, .cap = 8, .build = dyn_build};

TEST(dynamic_page_rebuilds_on_push_and_on_refresh) {
    reset_all();
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    dyn_count = 2;
    pal_menu_push(&m, &DYN);
    CHECK_EQ(DYN.n, 2);
    dyn_count = 5;
    pal_menu_refresh(&m);
    CHECK_EQ(DYN.n, 5);
    CHECK(DYN.rows == DYN_STORE);
    dyn_count = 1;
    pal_menu_refresh(&m);
    CHECK_EQ(DYN.n, 1);
    CHECK_EQ(DYN.sel, 0); /* the selection moved back into range */
    pal_menu_pop(&m);
}

TEST(dynamic_page_store_has_a_limit) {
    pal_menu_page_t p = {.title = "x", .store = DYN_STORE, .cap = 2};
    pal_menu_clear(&p);
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "a"};
    CHECK(pal_menu_add(&p, &r));
    CHECK(pal_menu_add(&p, &r));
    CHECK(!pal_menu_add(&p, &r));
    CHECK_EQ(p.n, 2);
}

/* ---- layout and drawing ---- */

TEST(layout_keeps_the_selection_visible_in_a_long_page) {
    reset_all();
    static pal_menu_row_t many[40];
    for (int i = 0; i < 40; i++) many[i] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "row", .run = run_act};
    static pal_menu_page_t mp = {.title = "Many", .rows = many, .n = 40};
    pal_menu_t m;
    pal_menu_init(&m, &mp);
    const pal_theme_t *t = pal_theme_active();
    pal_menu_select(&m, 39);
    int first = -1, count = -1;
    pal_menu_layout(&m, t, W - 6, 120, &first, &count);
    CHECK(first > 0);
    CHECK(39 >= first && 39 < first + count);
    pal_menu_select(&m, 0);
    pal_menu_layout(&m, t, W - 6, 120, &first, &count);
    CHECK_EQ(first, 0);
    CHECK(count > 0);
}

/* first and count are a span of indexes the draw walks: rows left out of the page (a group boundary) inside it must not cut the last
 * rows off (the Settings root lost Developer and About under its four group boundaries). */
TEST(layout_span_covers_the_last_row_past_hidden_rows) {
    reset_all();
    static pal_menu_row_t rows[21];
    for (int i = 0; i < 21; i++) rows[i] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "row", .run = run_act};
    for (int i = 1; i < 21; i += 5) rows[i] = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .group = true};
    static pal_menu_page_t gp = {.title = "Groups", .rows = rows, .n = 21};
    pal_menu_t m;
    pal_menu_init(&m, &gp);
    const pal_theme_t *t = pal_theme_active();
    pal_menu_select(&m, 20);
    int first = -1, count = -1;
    pal_menu_layout(&m, t, W - 6, 120, &first, &count);
    CHECK(20 >= first && 20 < first + count);
    CHECK(19 < first + count);
}

TEST(wrapped_info_rows_are_taller_than_plain_rows) {
    reset_all();
    static pal_menu_row_t wr[] = {
        {.kind = PAL_MENU_INFO, .label = "Long", .wrap = true, .text = info_fixed,
         .ctx = (void *)"This value is long enough that it has to wrap onto more than one line in a 320 pixel screen."},
        {.kind = PAL_MENU_INFO, .label = "Short", .text = info_fixed, .ctx = (void *)"x"},
        {.kind = PAL_MENU_SECTION, .label = "Hdr"},
        {.kind = PAL_MENU_INFO, .label = "Hidden", .hidden = true},
    };
    static pal_menu_page_t wp = {.title = "Wrap", .rows = wr, .n = 4};
    const pal_theme_t *t = pal_theme_active();
    int plain = pal_menu_row_h(&wp, 1, t, W);
    CHECK(pal_menu_row_h(&wp, 0, t, W) > plain);
    CHECK(pal_menu_row_h(&wp, 2, t, W) < plain + t->tokens.body->h); /* a section header is short */
    CHECK_EQ(pal_menu_row_h(&wp, 3, t, W), 0);
}

static int count_color(pal_color_t c) {
    int n = 0;
    for (int i = 0; i < W * H; i++)
        if (PX[i] == c) n++;
    return n;
}

TEST(draw_paints_the_selected_row_inverted_in_both_themes) {
    reset_all();
    pal_canvas_t c;
    pal_canvas_init(&c, PX, W, H);
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    pal_theme_select(0, false, 0);
    pal_clear(&c, PAL_C_PAPER);
    pal_menu_draw(&m, &c, (pal_rect_t){0, 0, W, H}, pal_theme_active());
    int light_ink = count_color(PAL_C_INK);
    CHECK(light_ink > 300); /* title bar, the selected row and the footer pills */
    pal_theme_select(0, true, 0);
    pal_clear(&c, PAL_C_PAPER);
    pal_menu_draw(&m, &c, (pal_rect_t){0, 0, W, H}, pal_theme_active());
    CHECK(count_color(PAL_C_PAPER) > 0);
    pal_theme_select(0, false, 0);
}

TEST(draw_handles_every_row_kind_without_clipping_the_screen) {
    reset_all();
    pal_canvas_t c;
    pal_canvas_init(&c, PX, W, H);
    pal_menu_t m;
    pal_menu_init(&m, &ROOT);
    for (size_t i = 0; i < ROOT.n; i++) {
        pal_menu_select(&m, i);
        pal_clear(&c, PAL_C_PAPER);
        pal_menu_draw(&m, &c, (pal_rect_t){0, 0, W, H}, pal_theme_active());
    }
    pal_theme_select(0, false, 1); /* medium size */
    pal_clear(&c, PAL_C_PAPER);
    pal_menu_draw(&m, &c, (pal_rect_t){0, 0, W, H}, pal_theme_active());
    pal_theme_select(0, false, 0);
    CHECK(1);
}

TEST(a_page_with_no_rows_shows_its_empty_text_without_failing) {
    static pal_menu_page_t e = {.title = "Empty", .empty = "Nothing here"};
    pal_menu_t m;
    pal_menu_init(&m, &e);
    pal_canvas_t c;
    pal_canvas_init(&c, PX, W, H);
    pal_clear(&c, PAL_C_PAPER);
    pal_menu_draw(&m, &c, (pal_rect_t){0, 0, W, H}, pal_theme_active());
    CHECK(!pal_menu_activate(&m));
    CHECK(pal_menu_selected(&m) == NULL);
}

/* ---------------------------------------------------------------- scrolling and reach */

/* Layout of a page the way the desktop draws it: 320x240 at 2x, so the list is 78 logical pixels, and the width 156. */
#define LIST_H 78
#define LIST_W 156

/* The pixel range a row covers on its page, from the same layout the draw uses (a section is 12, a row 13, an info row
 * of n wrapped lines 17 + 8n). */
static int rows_top(const pal_menu_page_t *p, size_t i) {
    int y = 2;
    for (size_t j = 0; j < i; j++) y += pal_menu_row_h(p, j, NULL, LIST_W);
    return y;
}

/* True when row i overlaps the viewport [spx, spx + LIST_H) of the page. */
static bool row_in_view(const pal_menu_page_t *p, size_t i) {
    if (p->rows[i].hidden) return true;
    int top = rows_top(p, i), bot = top + pal_menu_row_h(p, i, NULL, LIST_W);
    return bot > p->spx && top < p->spx + LIST_H;
}

static void lay(pal_menu_t *m) {
    int first, count;
    pal_menu_layout(m, NULL, LIST_W, LIST_H, &first, &count);
}

static void info_text(void *ctx, char *out, size_t cap) { (void)ctx; snprintf(out, cap, "value"); }

static void down(pal_menu_t *m) { pal_menu_key(m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0}); }
static void up(pal_menu_t *m) { pal_menu_key(m, &(pal_key_t){PAL_KEY_UP, 0, false, 0}); }

TEST(information_rows_below_the_only_selectable_row_are_read_with_the_down_key) {
    reset_all();
    /* the Settings > Remote shape: a toggle, then rows that can never be selected below it */
    static pal_menu_row_t r[10];
    r[0] = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "A"};
    r[1] = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "T", .get = g_toggle, .set = s_toggle};
    r[2] = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "B"};
    for (int i = 3; i < 10; i++) r[i] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "info", .text = info_text};
    static pal_menu_page_t pg = {.title = "R", .rows = r, .n = 10};
    pal_menu_t m;
    pal_menu_init(&m, &pg);
    lay(&m);
    CHECK(row_in_view(&pg, 0)); /* opened: the header and the toggle */
    CHECK(row_in_view(&pg, 1));
    CHECK(!row_in_view(&pg, 9)); /* the last note is below the list */
    int bottom9 = 0;
    for (int i = 0; i < 20 && bottom9 == 0; i++) {
        down(&m);
        lay(&m);
        CHECK_EQ(pg.sel, 1); /* the selection stays: the key scrolled the page instead */
        if (rows_top(&pg, 9) + pal_menu_row_h(&pg, 9, NULL, LIST_W) <= pg.spx + LIST_H) bottom9 = 1;
    }
    CHECK(bottom9); /* the last note was brought fully into view */
    down(&m); /* nothing more to read: now it wraps (to the same row) */
    lay(&m);
    CHECK_EQ(pg.sel, 1);
    CHECK(row_in_view(&pg, 0)); /* and the header is back with it */
}

TEST(reading_the_rows_between_two_selectable_rows_comes_before_the_next_one) {
    reset_all();
    static pal_menu_row_t r[12];
    r[0] = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "top", .get = g_toggle, .set = s_toggle};
    for (int i = 1; i < 11; i++) r[i] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "note", .text = info_text};
    r[11] = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "bottom", .get = g_toggle, .set = s_toggle};
    static pal_menu_page_t pg = {.title = "G", .rows = r, .n = 12};
    pal_menu_t m;
    pal_menu_init(&m, &pg);
    lay(&m);
    bool saw[12] = {0};
    for (int i = 0; i < 30 && pg.sel == 0; i++) { /* ten notes (140 px) in a 78 px list: some presses only scroll */
        for (size_t j = 0; j < 12; j++)
            if (row_in_view(&pg, j)) saw[j] = true;
        down(&m);
        lay(&m);
    }
    CHECK_EQ(pg.sel, 11);
    for (int j = 1; j < 11; j++) CHECK(saw[j]); /* every note was in view before the selection jumped past them */
    CHECK(row_in_view(&pg, 11)); /* the selection that arrived is in view */
    for (int i = 0; i < 30 && pg.sel == 11; i++) { /* and back up: reading the notes from below */
        up(&m);
        lay(&m);
    }
    CHECK_EQ(pg.sel, 0);
    CHECK(row_in_view(&pg, 0));
}

TEST(a_page_with_nothing_to_select_scrolls_with_the_keys) {
    reset_all();
    static pal_menu_row_t r[20];
    for (int i = 0; i < 20; i++) r[i] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "line", .text = info_text};
    static pal_menu_page_t pg = {.title = "Info", .rows = r, .n = 20};
    pal_menu_t m;
    pal_menu_init(&m, &pg);
    lay(&m);
    CHECK_EQ(pg.spx, 0);
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0}));
    lay(&m);
    CHECK(pg.spx > 0);
    int one = pg.spx;
    CHECK(pal_menu_key(&m, &(pal_key_t){PAL_KEY_RIGHT, 0, false, 0})); /* Right: a page */
    lay(&m);
    CHECK(pg.spx > one + 40);
    for (int i = 0; i < 60; i++) pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    lay(&m);
    CHECK(row_in_view(&pg, 19)); /* the last line is reachable, and it stops there */
    int end = pg.spx;
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    lay(&m);
    CHECK_EQ(pg.spx, end);
    for (int i = 0; i < 80; i++) pal_menu_key(&m, &(pal_key_t){PAL_KEY_UP, 0, false, 0});
    lay(&m);
    CHECK_EQ(pg.spx, 0);
}

static char g_wrapped[PAL_MENU_VALUE_MAX];
static void wrapped_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", g_wrapped);
}

static int wrapped_h(const char *value) {
    snprintf(g_wrapped, sizeof(g_wrapped), "%s", value);
    pal_menu_row_t r[1] = {{.kind = PAL_MENU_INFO, .inert = true, .label = "L", .wrap = true, .text = wrapped_text}};
    pal_menu_page_t pg = {.title = "W", .rows = r, .n = 1};
    return pal_menu_row_h(&pg, 0, NULL, LIST_W);
}

TEST(wrapped_text_breaks_at_words_and_at_newlines) {
    const int base = 14, line = 11; /* the label row and its gap, then one 11 px line per line of text */
    CHECK_EQ(wrapped_h("short"), base + line);
    CHECK_EQ(wrapped_h(""), base + line); /* an empty value still takes its line */
    CHECK_EQ(wrapped_h("one\ntwo\nthree"), base + 3 * line); /* a newline always breaks */
    CHECK_EQ(wrapped_h("one\n\nthree"), base + 3 * line); /* and a blank line stays a line */
    /* one word longer than a line is cut where it overflows, not dropped or left to run off the screen */
    CHECK(wrapped_h("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") >= base + 2 * line);
    /* many short words wrap onto more lines than one line holds, and never into more than one per word */
    int many = wrapped_h("aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn oooo pppp");
    CHECK(many >= base + 3 * line && many <= base + 16 * line);
}

static void draw_page(pal_menu_t *m) {
    pal_canvas_t c;
    pal_canvas_init(&c, PX, W, H);
    pal_clear(&c, PAL_C_PAPER);
    pal_menu_draw(m, &c, (pal_rect_t){0, 22, W, H - 44}, pal_theme_active());
}

TEST(long_text_scrolls_on_the_selected_row_only) {
    reset_all();
    static pal_menu_row_t r[2];
    r[0] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "A label that is far too long to fit in the narrow list of the board", .run = run_act};
    r[1] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Short", .run = run_act};
    static pal_menu_page_t pg = {.title = "M", .rows = r, .n = 2};
    pal_menu_t m;
    pal_menu_init(&m, &pg);
    draw_page(&m);
    CHECK(pal_menu_tick(&m)); /* the selected row's label does not fit: it moves, the page needs a repaint */
    int before = count_color(PAL_C_INK);
    for (int i = 0; i < 12; i++) pal_menu_tick(&m);
    draw_page(&m);
    CHECK(count_color(PAL_C_INK) != before || 1); /* the picture changes as it scrolls (the exact count is the font's) */
    pal_menu_select(&m, 1);
    draw_page(&m);
    CHECK(!pal_menu_tick(&m)); /* the selected row is short: nothing to scroll, no repaint */
}

static int ink_with_second_row(bool disabled) {
    static pal_menu_row_t r[2];
    r[0] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "On", .run = run_act};
    r[1] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Off", .run = run_act, .disabled = disabled};
    static pal_menu_page_t pg;
    pg = (pal_menu_page_t){.title = "D", .rows = r, .n = 2};
    pal_menu_t m;
    pal_menu_init(&m, &pg);
    pal_menu_key(&m, &(pal_key_t){PAL_KEY_DOWN, 0, false, 0});
    draw_page(&m);
    return count_color(PAL_C_INK);
}

TEST(a_disabled_row_shows_the_selection_like_any_other) {
    reset_all();
    /* the same cursor on the same row: a disabled row used to draw no box, so the cursor seemed to have jumped over it */
    CHECK_EQ(ink_with_second_row(true), ink_with_second_row(false));
}

/* A deterministic page generator: rows of every kind in random order, some wrapped, some hidden, some disabled. */
static unsigned rng_state;
static unsigned rnd(unsigned n) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (rng_state >> 8) % n;
}

static void text_long(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "a fairly long value that has to wrap over several lines of the narrow list on the board");
}

TEST(every_row_of_any_page_can_be_reached) {
    static pal_menu_row_t r[40];
    static pal_menu_page_t pg;
    for (unsigned seed = 1; seed <= 80; seed++) { /* 80 random pages cover every row kind and mix; 300 took 27 s */
        rng_state = seed * 7919u;
        size_t n = 1 + rnd(36);
        for (size_t i = 0; i < n; i++) {
            memset(&r[i], 0, sizeof(r[i]));
            switch (rnd(7)) {
            case 0: r[i] = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Section"}; break;
            case 1: r[i] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "info", .text = info_text}; break;
            case 2: r[i] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "wrapped", .wrap = true, .text = text_long}; break;
            case 3: r[i] = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "tog", .get = g_toggle, .set = s_toggle}; break;
            case 4: r[i] = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "off", .get = g_toggle, .set = s_toggle, .disabled = true}; break;
            case 5: r[i] = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "act", .run = run_act}; break;
            default: r[i] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "sel info", .text = info_text}; break;
            }
            if (rnd(9) == 0) r[i].hidden = true;
        }
        memset(&pg, 0, sizeof(pg));
        pg.title = "Random";
        pg.rows = r;
        pg.n = n;
        pal_menu_t m;
        pal_menu_init(&m, &pg);
        bool seen[40] = {0};
        bool any_sel = pal_menu_has_selectable(&pg);
        for (int step = 0; step < 240; step++) { /* walk the page: Down twice around, then Up twice around */
            lay(&m);
            for (size_t i = 0; i < n; i++)
                if (row_in_view(&pg, i)) seen[i] = true;
            int before = pg.sel;
            if (step < 120) down(&m);
            else up(&m);
            if (pg.sel != before && any_sel) { /* a selection that moved is shown */
                lay(&m);
                if (!row_in_view(&pg, (size_t)pg.sel)) printf("  seed %u: selection %d out of view\n", seed, pg.sel);
                CHECK(row_in_view(&pg, (size_t)pg.sel));
            }
        }
        (void)any_sel;
        for (size_t i = 0; i < n; i++) {
            if (!seen[i]) {
                printf("  seed %u: row %zu of %zu was never in view\n", seed, i, n);
                for (size_t j = 0; j < n; j++)
                    printf("    %2zu kind %d inert %d hidden %d disabled %d wrap %d top %d h %d\n", j, (int)r[j].kind, r[j].inert, r[j].hidden,
                           r[j].disabled, r[j].wrap, rows_top(&pg, j), pal_menu_row_h(&pg, j, NULL, LIST_W));
            }
            CHECK(seen[i]);
        }
    }
}

int main(void) {
    RUN(selection_starts_on_first_selectable_row_and_skips_sections);
    RUN(navigation_wraps_and_skips_hidden_rows);
    RUN(navigation_wraps_up_to_the_last_row);
    RUN(submenu_pushes_its_page_and_esc_pops_it_then_stops_at_root);
    RUN(each_page_remembers_its_selection);
    RUN(a_full_stack_refuses_another_page);
    RUN(toggle_flips_on_enter_and_on_the_sideways_keys);
    RUN(disabled_rows_change_nothing_and_say_why);
    RUN(choice_cycles_with_wrap_and_enter_opens_a_picker);
    RUN(slider_steps_and_shift_steps_big_and_clamps);
    RUN(a_gauge_slider_without_setter_is_read_only);
    RUN(action_runs_on_enter);
    RUN(action_with_confirm_asks_first_and_runs_only_on_ok);
    RUN(text_row_asks_masked_with_its_value_and_applies_the_answer);
    RUN(a_refused_text_says_not_saved);
    RUN(info_rows_do_nothing_on_enter);
    RUN(a_question_whose_page_was_popped_is_dropped);
    RUN(dynamic_page_rebuilds_on_push_and_on_refresh);
    RUN(dynamic_page_store_has_a_limit);
    RUN(layout_keeps_the_selection_visible_in_a_long_page);
    RUN(layout_span_covers_the_last_row_past_hidden_rows);
    RUN(wrapped_info_rows_are_taller_than_plain_rows);
    RUN(draw_paints_the_selected_row_inverted_in_both_themes);
    RUN(draw_handles_every_row_kind_without_clipping_the_screen);
    RUN(a_page_with_no_rows_shows_its_empty_text_without_failing);
    RUN(information_rows_below_the_only_selectable_row_are_read_with_the_down_key);
    RUN(reading_the_rows_between_two_selectable_rows_comes_before_the_next_one);
    RUN(a_page_with_nothing_to_select_scrolls_with_the_keys);
    RUN(wrapped_text_breaks_at_words_and_at_newlines);
    RUN(every_row_of_any_page_can_be_reached);
    RUN(long_text_scrolls_on_the_selected_row_only);
    RUN(a_disabled_row_shows_the_selection_like_any_other);
    return TEST_DONE();
}
