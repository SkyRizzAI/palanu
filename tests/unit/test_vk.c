/* The on-screen keyboard (system/src/vk*.c): layout and focus on the grid, the shift and sticky rules, the translation of
 * every key, the coverage of printable ASCII, the text helpers, source mirroring with a fake source and a fake injection
 * hook, and the entry mode (a fake overlay stack and a fake notify through the async callback). The desktop is replaced by
 * hooks; the test thread plays the desktop task when it runs the view's pump. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/input.h"
#include "pal/vk.h"
#include "pal/vk_layout.h"
#include "test.h"
#include "../../system/src/vk_int.h"

static uint32_t g_clk;
static uint32_t fake_clock(void) { return g_clk; }
static pal_key_t g_got[64];
static int g_ngot, g_pushes, g_pops;
static void inject_hook(const pal_key_t *k) {
    if (g_ngot < 64) g_got[g_ngot++] = *k;
}
static nema_err_t push_fake(pal_view_t *v) {
    (void)v;
    g_pushes++;
    return NEMA_OK;
}
static void pop_fake(pal_view_t *v) {
    (void)v;
    g_pops++;
}
static bool present_yes(void) { return true; }
static bool present_no(void) { return false; }
static void wake_nop(void) {}
static pal_vk_hooks_t HOOKS = {push_fake, pop_fake, present_yes, inject_hook, fake_clock, wake_nop};

/* A fresh keyboard: nothing open, no sources, the fake desktop. */
static void reset(void) {
    memset(&vk_S, 0, sizeof(vk_S));
    vk_S.flash = -1;
    pal_vk_set_hooks(&HOOKS);
    g_pushes = g_pops = g_ngot = 0;
    g_clk = 1000;
}

static void press_key(pal_key_t k) { (void)pal_vk_view.key(&pal_vk_view, &k); }
static void press_char(uint32_t cp) {
    pal_key_t k = {PAL_KEY_CHAR, 0, false, cp};
    press_key(k);
}
static void press_sym(uint8_t sym) {
    pal_key_t k = {sym, 0, false, 0};
    press_key(k);
}

/* The index of the first cell whose key is a character cp (layer as built now). */
static int cell_of_char(uint32_t cp) {
    for (int i = 0; i < vk_S.ncells; i++)
        if (vk_S.cells[i].key->kind == VK_K_CHAR && vk_S.cells[i].key->cp == cp) return i;
    return -1;
}
static int cell_of_kind(uint8_t kind) {
    for (int i = 0; i < vk_S.ncells; i++)
        if (vk_S.cells[i].key->kind == kind) return i;
    return -1;
}

static void set_ui(int layer, int entry, int mask) {
    pal_vk_set_hooks(&HOOKS);
    g_ngot = 0;
    memset(&vk_S.ui, 0, sizeof(vk_S.ui));
    vk_S.ui.layer = (uint8_t)layer;
    vk_S.ui.entry = (uint8_t)entry;
    vk_S.ui.mask = (uint8_t)mask;
    vk_S.ui.show = 0;
    int n = vk_build(&vk_S.ui, vk_S.cells, VK_MAX_CELLS);
    vk_S.ncells = n;
    vk_S.focus = vk_focus_home(vk_S.cells, n, entry != 0);
    vk_S.pref2 = vk_pref2(vk_S.cells, vk_S.focus);
}

/* ---- layout ---- */
TEST(every_row_is_twenty_half_units_in_every_layer) {
    for (int layer = 0; layer < 4; layer++)
        for (int variant = 0; variant < 3; variant++) {
            vk_ui_t u;
            memset(&u, 0, sizeof(u));
            u.layer = (uint8_t)layer;
            u.entry = variant != 0;
            u.mask = variant == 1;
            vk_cell_t c[VK_MAX_CELLS];
            int n = vk_build(&u, c, VK_MAX_CELLS);
            CHECK(n > 0);
            int sum[VK_ROWS] = {0, 0, 0, 0};
            for (int i = 0; i < n; i++) sum[c[i].row] += c[i].w;
            for (int r = 0; r < VK_ROWS; r++) {
                if (sum[r] != VK_ROW_HU) printf("    layer %d variant %d row %d sums to %d\n", layer, variant, r, sum[r]);
                CHECK_EQ(sum[r], VK_ROW_HU);
            }
        }
}

TEST(layer_key_counts_and_focus_home) {
    set_ui(VKL_LETTERS, 0, 0);
    CHECK_EQ(vk_S.ncells, 34);
    /* the focus starts on the layer key at the left of the bottom row ("123"), where the layer switch is one Enter away */
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_LAYER);
    CHECK_EQ(vk_S.cells[vk_S.focus].row, vk_S.cells[vk_S.ncells - 1].row);
    CHECK(vk_S.cells[vk_S.focus].x <= 2);
    set_ui(VKL_SYM, 0, 0);
    CHECK_EQ(vk_S.ncells, 35); /* the bottom row of 123 has six keys */
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_LAYER);
    set_ui(VKL_CTL, 0, 0);
    CHECK_EQ(vk_S.ncells, 29);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_LAYER);
    set_ui(VKL_LETTERS, 1, 0); /* entry mode keeps the reference home: the middle of the second row, g */
    CHECK_EQ(vk_S.cells[vk_S.focus].key->cp, 'g');
    set_ui(VKL_LETTERS, 1, 1); /* masked entry: the reference bottom row, no eye key */
    CHECK_EQ(cell_of_kind(VK_K_SHOW), -1);
    CHECK(cell_of_kind(VK_K_OK) >= 0);
    CHECK(cell_of_kind(VK_K_CANCEL) >= 0);
}

TEST(left_right_wrap_within_the_row) {
    set_ui(VKL_LETTERS, 0, 0);
    int q = cell_of_char('q'), p = cell_of_char('p');
    vk_S.focus = q;
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_LEFT, &vk_S.pref2);
    CHECK_EQ(vk_S.focus, p);
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_RIGHT, &vk_S.pref2);
    CHECK_EQ(vk_S.focus, q);
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_END, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->cp, 'p');
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_HOME, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->cp, 'q');
}

TEST(up_wraps_to_the_bottom_row_nearest_the_column) {
    set_ui(VKL_LETTERS, 0, 0);
    vk_S.focus = cell_of_char('q');
    vk_S.pref2 = vk_pref2(vk_S.cells, vk_S.focus);
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_UP, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].row, 3);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_LAYER); /* the 123 key under q */
    CHECK_EQ(vk_S.cells[vk_S.focus].key->layer, VKL_SYM);
}

TEST(vertical_moves_keep_the_column) {
    set_ui(VKL_LETTERS, 0, 0);
    vk_S.focus = cell_of_char('p');
    vk_S.pref2 = vk_pref2(vk_S.cells, vk_S.focus); /* centre 38 */
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_DOWN, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->cp, '\''); /* row 1 under p */
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_DOWN, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_BACK); /* row 2: the backspace (centre 37) */
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_DOWN, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_ENTER); /* bottom row: Enter (centre 35) */
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_UP, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_BACK);
    CHECK_EQ(vk_S.pref2, 38); /* the column did not move */
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_UP, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->cp, '\''); /* back on the row above */
}

TEST(page_keys_jump_to_the_top_and_bottom_rows) {
    set_ui(VKL_LETTERS, 0, 0);
    vk_S.focus = cell_of_char('a');
    vk_S.pref2 = vk_pref2(vk_S.cells, vk_S.focus);
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_PGDN, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].row, 3);
    vk_S.focus = vk_move(vk_S.cells, vk_S.ncells, vk_S.focus, VK_DIR_PGUP, &vk_S.pref2);
    CHECK_EQ(vk_S.cells[vk_S.focus].row, 0);
}

/* ---- shift, layers and sticky modifiers (pure rules) ---- */
TEST(shift_latches_then_double_tap_locks) {
    vk_ui_t u;
    memset(&u, 0, sizeof(u));
    vk_shift_tap(&u, 100);
    CHECK_EQ(u.shift, VK_SHIFT_ONE);
    vk_shift_tap(&u, 250);
    CHECK_EQ(u.shift, VK_SHIFT_CAPS);
    vk_shift_tap(&u, 400);
    CHECK_EQ(u.shift, VK_SHIFT_OFF);
    vk_shift_tap(&u, 1000); /* a single tap */
    CHECK_EQ(u.shift, VK_SHIFT_ONE);
    vk_shift_tap(&u, 3000); /* the second tap came too late to be a double tap: off */
    CHECK_EQ(u.shift, VK_SHIFT_OFF);
}

TEST(tab_cycles_the_layers_and_entry_skips_ctl) {
    vk_ui_t u;
    memset(&u, 0, sizeof(u));
    vk_cycle_layer(&u);
    CHECK_EQ(u.layer, VKL_SYM);
    vk_cycle_layer(&u);
    CHECK_EQ(u.layer, VKL_SYM2);
    vk_cycle_layer(&u);
    CHECK_EQ(u.layer, VKL_CTL);
    vk_cycle_layer(&u);
    CHECK_EQ(u.layer, VKL_LETTERS);
    u.entry = 1;
    u.layer = VKL_SYM2;
    vk_cycle_layer(&u);
    CHECK_EQ(u.layer, VKL_LETTERS);
}

TEST(upper_case_follows_shift_and_physical_shift) {
    vk_ui_t u;
    memset(&u, 0, sizeof(u));
    CHECK(!vk_is_upper(&u, 0));
    CHECK(vk_is_upper(&u, PAL_MOD_SHIFT));
    u.shift = VK_SHIFT_ONE;
    CHECK(vk_is_upper(&u, 0));
    CHECK_EQ(vk_upper('a'), 'A');
    CHECK_EQ(vk_upper('1'), '1');
}

TEST(sticky_ctrl_and_alt_are_applied_once) {
    set_ui(VKL_CTL, 0, 0);
    int ctrl = cell_of_kind(VK_K_MOD);
    CHECK(ctrl >= 0);
    vk_press(ctrl);
    CHECK_EQ(vk_S.ui.sticky, VK_MOD_CTRL);
    /* the ^C chord, with Ctrl latched */
    int chord = -1;
    for (int i = 0; i < vk_S.ncells; i++)
        if (vk_S.cells[i].key->kind == VK_K_CHORD && vk_S.cells[i].key->cp == 'c') chord = i;
    vk_press(chord);
    CHECK_EQ(g_ngot, 1);
    CHECK_EQ(g_got[0].sym, PAL_KEY_CHAR);
    CHECK_EQ(g_got[0].cp, 'c');
    CHECK(g_got[0].mods & PAL_MOD_CTRL);
    CHECK_EQ(vk_S.ui.sticky, 0); /* released after the key */
}

TEST(one_shot_shift_is_used_by_one_letter) {
    set_ui(VKL_LETTERS, 0, 0);
    g_ngot = 0;
    vk_S.ui.shift = VK_SHIFT_ONE;
    vk_press(cell_of_char('a'));
    CHECK_EQ(g_ngot, 1);
    CHECK_EQ(g_got[0].cp, 'A');
    CHECK(g_got[0].mods & PAL_MOD_SHIFT);
    CHECK_EQ(vk_S.ui.shift, VK_SHIFT_OFF);
    vk_press(cell_of_char('b'));
    CHECK_EQ(g_got[1].cp, 'b');
    vk_S.ui.shift = VK_SHIFT_CAPS;
    vk_press(cell_of_char('c'));
    vk_press(cell_of_char('d'));
    CHECK_EQ(g_got[3].cp, 'D'); /* caps lock stays */
}

/* ---- the key table: every key's translation ---- */
TEST(key_to_event_translation_table) {
    pal_key_t e;
    vk_key_t ch = {VK_K_CHAR, 2, 0, 0, 0, 'x', NULL};
    CHECK(vk_key_event(&ch, 0, &e));
    CHECK_EQ(e.sym, PAL_KEY_CHAR);
    CHECK_EQ(e.cp, 'x');
    CHECK_EQ(e.mods, 0);
    CHECK(vk_key_event(&ch, 1, &e));
    CHECK_EQ(e.cp, 'X');
    CHECK_EQ(e.mods, PAL_MOD_SHIFT);
    vk_key_t back = {VK_K_BACK, 3, 0, 0, 0, 0, NULL};
    CHECK(vk_key_event(&back, 0, &e));
    CHECK_EQ(e.sym, PAL_KEY_BACKSPACE);
    vk_key_t enter = {VK_K_ENTER, 5, 0, 0, 0, 0, NULL};
    CHECK(vk_key_event(&enter, 0, &e));
    CHECK_EQ(e.sym, PAL_KEY_ENTER);
    vk_key_t ok = {VK_K_OK, 5, 0, 0, 0, 0, "OK"};
    CHECK(vk_key_event(&ok, 0, &e));
    CHECK_EQ(e.sym, PAL_KEY_ENTER);
    vk_key_t tab = {VK_K_SPECIAL, 2, 0, 0, PAL_KEY_TAB, 0, "Tab"};
    CHECK(vk_key_event(&tab, 0, &e));
    CHECK_EQ(e.sym, PAL_KEY_TAB);
    vk_key_t ins = {VK_K_SPECIAL, 2, 0, 0, PAL_KEY_INSERT, 0, "Ins"};
    CHECK(vk_key_event(&ins, 0, &e));
    CHECK_EQ(e.sym, PAL_KEY_INSERT);
    vk_key_t chord = {VK_K_CHORD, 2, 0, 0, 0, 'c', "^C"};
    CHECK(vk_key_event(&chord, 0, &e));
    CHECK_EQ(e.sym, PAL_KEY_CHAR);
    CHECK_EQ(e.cp, 'c');
    CHECK_EQ(e.mods, PAL_MOD_CTRL);
    /* keys that only change the keyboard send nothing */
    vk_key_t shift = {VK_K_SHIFT, 3, 0, 0, 0, 0, NULL};
    CHECK(!vk_key_event(&shift, 0, &e));
    vk_key_t layer = {VK_K_LAYER, 3, VKL_SYM, 0, 0, 0, "123"};
    CHECK(!vk_key_event(&layer, 0, &e));
    vk_key_t mod = {VK_K_MOD, 2, 0, VK_MOD_ALT, 0, 0, "Alt"};
    CHECK(!vk_key_event(&mod, 0, &e));
    vk_key_t show = {VK_K_SHOW, 2, 0, 0, 0, 0, NULL};
    CHECK(!vk_key_event(&show, 0, &e));
}

TEST(every_printable_ascii_character_is_on_the_keyboard) {
    /* the characters the physical keyboard can send (docs/KEYMAP.md) are all reachable here: letters with shift, and the
     * symbols of the 123 and #+= layers */
    bool have[128] = {false};
    for (int layer = 0; layer < 4; layer++) {
        for (int variant = 0; variant < 3; variant++) {
            vk_ui_t u;
            memset(&u, 0, sizeof(u));
            u.layer = (uint8_t)layer;
            u.entry = variant != 0;
            u.mask = variant == 1;
            vk_cell_t c[VK_MAX_CELLS];
            int n = vk_build(&u, c, VK_MAX_CELLS);
            for (int i = 0; i < n; i++)
                if (c[i].key->kind == VK_K_CHAR && c[i].key->cp < 128) {
                    have[c[i].key->cp] = true;
                    have[vk_upper(c[i].key->cp)] = true; /* shift gives the capitals */
                }
        }
    }
    int missing = 0;
    for (int cp = 0x20; cp <= 0x7E; cp++)
        if (!have[cp]) {
            missing++;
            printf("    missing '%c'\n", cp);
        }
    CHECK_EQ(missing, 0);
}

/* ---- text helpers ---- */
TEST(text_insert_backspace_and_counting) {
    char buf[16] = "";
    size_t n = vk_text_insert(buf, 0, sizeof(buf), 'a');
    n = vk_text_insert(buf, n, sizeof(buf), 0x20AC); /* euro sign: three bytes */
    CHECK_EQ(n, 4);
    CHECK_EQ(vk_cp_count(buf, n), 2);
    CHECK_EQ(vk_cp_index(buf, n, 1), 1);
    CHECK_EQ(vk_cp_index(buf, n, 4), 2);
    n = vk_text_backspace(buf, n);
    CHECK_EQ(n, 1);
    CHECK_EQ(buf[n], 0);
    char small[3] = "";
    size_t m = vk_text_insert(small, 0, sizeof(small), 'x');
    m = vk_text_insert(small, m, sizeof(small), 'y'); /* no room for the NUL: refused */
    CHECK_EQ(m, 2); /* two bytes and the NUL fill the three */
    m = vk_text_insert(small, m, sizeof(small), 'z');
    CHECK_EQ(m, 2); /* no room: refused */
    CHECK_EQ(vk_text_backspace(buf, 0), 0);
}

TEST(caret_scrolls_to_stay_visible) {
    CHECK_EQ(vk_scroll_for(0, 10, 5), 6);
    CHECK_EQ(vk_scroll_for(5, 2, 5), 2);
    CHECK_EQ(vk_scroll_for(3, 4, 5), 3);
    CHECK_EQ(vk_scroll_for(3, 7, 5), 3);
    CHECK_EQ(vk_scroll_for(3, 8, 5), 4);
}

/* ---- source mirroring ---- */
static char g_src_text[64];
static size_t g_src_cur;
static bool g_src_active;
static int fake_source(void *ctx, char *out, size_t cap, size_t *cursor) {
    (void)ctx;
    if (!g_src_active) return -1;
    size_t n = strlen(g_src_text);
    if (n + 1 > cap) n = cap - 1;
    memcpy(out, g_src_text, n);
    out[n] = 0;
    if (cursor) *cursor = g_src_cur;
    return (int)n;
}
static pal_vk_source_t g_src = {fake_source, NULL};

TEST(box_mirrors_the_app_line_and_keys_reach_the_app) {
    reset();
    CHECK_EQ(pal_vk_register_source("", &g_src), NEMA_OK); /* the front view is "" here: no desktop */
    strcpy(g_src_text, "ls");
    g_src_cur = 2;
    g_src_active = true;
    CHECK_EQ(pal_vk_open(), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view); /* the desktop task applies the request */
    CHECK_EQ(g_pushes, 1);
    CHECK(pal_vk_is_open());
    CHECK_EQ(vk_S.len, 2);
    CHECK(memcmp(vk_S.text, "ls", 2) == 0);
    CHECK_EQ(vk_S.cursor, 2);
    /* a key goes to the app at once; the shell echoes it a little later */
    press_char('x');
    CHECK_EQ(g_ngot, 1);
    CHECK_EQ(g_got[0].cp, 'x');
    strcpy(g_src_text, "lsx");
    g_src_cur = 3;
    g_clk += 60;
    pal_vk_view.tick(&pal_vk_view, g_clk);
    CHECK_EQ(vk_S.len, 3);
    CHECK(memcmp(vk_S.text, "lsx", 3) == 0);
    /* no edit state on the app side: the box shows what was typed in this session */
    g_src_active = false;
    g_clk += 1000;
    pal_vk_view.tick(&pal_vk_view, g_clk);
    CHECK_EQ(vk_S.len, 1);
    CHECK(vk_S.text[0] == 'x');
    press_sym(PAL_KEY_ESC);
    CHECK(!pal_vk_is_open());
    CHECK_EQ(g_pops, 1);
    CHECK_EQ(pal_vk_register_source("", &g_src), NEMA_OK);
    vk_S.src_id[0] = NULL; /* keep the static tables tidy for the next test */
}

TEST(on_screen_keys_send_like_the_physical_keyboard) {
    reset();
    CHECK_EQ(pal_vk_open_text("", "abc"), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    CHECK(pal_vk_is_open());
    g_ngot = 0;
    vk_press(cell_of_char('k'));
    CHECK_EQ(g_ngot, 1);
    CHECK_EQ(g_got[0].cp, 'k');
    /* Enter presses the focused key: the focus starts on the layer key at the bottom left, so it switches to the symbols and
     * sends nothing, and after the switch the focus is on the layer key again */
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_LAYER);
    press_sym(PAL_KEY_ENTER);
    CHECK_EQ(vk_S.ui.layer, VKL_SYM);
    CHECK_EQ(g_ngot, 1);
    CHECK_EQ(vk_S.cells[vk_S.focus].key->kind, VK_K_LAYER);
    /* Tab cycles; Esc closes */
    press_sym(PAL_KEY_TAB);
    CHECK_EQ(vk_S.ui.layer, VKL_SYM2);
    press_sym(PAL_KEY_ESC);
    CHECK(!pal_vk_is_open());
}

TEST(the_enter_key_sends_enter_and_closes_the_keyboard) {
    reset();
    CHECK_EQ(pal_vk_open_text("", "abc"), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    CHECK(pal_vk_is_open());
    g_ngot = 0;
    vk_press(cell_of_char('l'));
    vk_press(cell_of_char('s'));
    CHECK_EQ(g_ngot, 2);
    vk_press(cell_of_kind(VK_K_ENTER)); /* the on-screen Enter: the line runs and the typing session ends */
    CHECK_EQ(g_ngot, 3);
    CHECK_EQ(g_got[2].sym, PAL_KEY_ENTER);
    CHECK(!pal_vk_is_open());
    CHECK_EQ(g_pops, 1);
}

TEST(fixed_text_does_not_follow_the_app) {
    reset();
    CHECK_EQ(pal_vk_open_text("ls -la /apps", "ABC"), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    CHECK_EQ(vk_S.len, 12);
    CHECK(vk_S.fixed);
    CHECK_EQ(vk_S.ui.shift, VK_SHIFT_ONE);
    CHECK_EQ(vk_S.ui.layer, VKL_LETTERS);
    g_src_active = true;
    strcpy(g_src_text, "other");
    g_clk += 2000;
    pal_vk_view.tick(&pal_vk_view, g_clk);
    CHECK_EQ(vk_S.len, 12); /* the source is not read */
    CHECK(vk_S.caret_on);   /* scenes: the caret does not blink */
}

/* ---- entry mode ---- */
static int g_cb_calls, g_cb_len;
static void entry_cb(int len, void *ctx) {
    (void)ctx;
    g_cb_calls++;
    g_cb_len = len;
}

TEST(entry_confirm_returns_the_text) {
    reset();
    char buf[16] = "old";
    pal_vk_entry_t e = {"Wi-Fi password", "", buf, sizeof(buf), true, false};
    g_cb_calls = 0;
    CHECK_EQ(pal_vk_edit_async(&e, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    CHECK_EQ(g_pushes, 1);
    CHECK(vk_S.ui.entry);
    CHECK(vk_S.ui.mask);
    press_char('S');
    press_char('3');
    press_char('!');
    CHECK_EQ(vk_S.len, 3);
    CHECK_EQ(g_ngot, 0); /* nothing goes to the app */
    press_sym(PAL_KEY_ENTER);
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(g_cb_len, 3);
    CHECK(strcmp(buf, "S3!") == 0);
    CHECK(!pal_vk_is_open());
    CHECK_EQ(g_pops, 1);
}

/* The owner's case: a Wi-Fi password with a '!'. The arrows reach '!' on the symbol layer and Enter types it; the
 * keyboard stays open. Typing on the physical keyboard then Enter confirms. */
TEST(enter_types_the_key_the_arrows_chose_in_an_entry) {
    reset();
    char buf[16] = "";
    pal_vk_entry_t e = {"Wi-Fi password", "", buf, sizeof(buf), true, false};
    g_cb_calls = 0;
    CHECK_EQ(pal_vk_edit_async(&e, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    press_char('a');
    press_sym(PAL_KEY_TAB); /* next layer: symbols */
    int bang = cell_of_char('!');
    for (int i = 0; i < 64 && bang >= 0 && vk_S.focus != bang; i++) {
        int fr = vk_S.cells[vk_S.focus].row, br = vk_S.cells[bang].row;
        if (fr < br) press_sym(PAL_KEY_DOWN);
        else if (fr > br) press_sym(PAL_KEY_UP);
        else if (vk_S.focus < bang) press_sym(PAL_KEY_RIGHT);
        else press_sym(PAL_KEY_LEFT);
    }
    CHECK(bang >= 0);
    CHECK_EQ(vk_S.focus, bang);
    press_sym(PAL_KEY_ENTER);
    CHECK(pal_vk_is_open());
    CHECK_EQ(g_cb_calls, 0);
    CHECK_EQ(vk_S.len, 2);
    CHECK(vk_S.text[1] == '!');
    press_sym(PAL_KEY_ENTER); /* still on '!': a second one */
    CHECK_EQ(vk_S.len, 3);
    press_char('Z');
    press_sym(PAL_KEY_ENTER); /* after typing: confirm */
    CHECK_EQ(g_cb_calls, 1);
    CHECK(strcmp(buf, "a!!Z") == 0);
    CHECK(!pal_vk_is_open());
}

TEST(entry_cancel_leaves_the_buffer) {
    reset();
    char buf[16] = "keep";
    pal_vk_entry_t e = {"t", NULL, buf, sizeof(buf), false, true};
    g_cb_calls = 0;
    CHECK_EQ(pal_vk_edit_async(&e, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    press_char('x');
    press_sym(PAL_KEY_ESC);
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(g_cb_len, PAL_VK_CANCEL);
    CHECK(strcmp(buf, "keep") == 0);
}

TEST(entry_empty_rule) {
    reset();
    char buf[8] = "";
    pal_vk_entry_t refuse = {"t", NULL, buf, sizeof(buf), false, false};
    g_cb_calls = 0;
    CHECK_EQ(pal_vk_edit_async(&refuse, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    vk_press(cell_of_kind(VK_K_OK));
    CHECK_EQ(g_cb_calls, 0); /* empty is refused: still open */
    CHECK(pal_vk_is_open());
    press_sym(PAL_KEY_ESC);
    CHECK_EQ(g_cb_len, PAL_VK_CANCEL);

    reset();
    pal_vk_entry_t allow = {"t", NULL, buf, sizeof(buf), false, true};
    g_cb_calls = 0;
    CHECK_EQ(pal_vk_edit_async(&allow, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    vk_press(cell_of_kind(VK_K_OK));
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(g_cb_len, 0);
    CHECK(buf[0] == 0);
}

TEST(entry_limit_is_cap_minus_one) {
    reset();
    char buf[4] = "";
    pal_vk_entry_t e = {"t", NULL, buf, sizeof(buf), false, true};
    g_cb_calls = 0;
    CHECK_EQ(pal_vk_edit_async(&e, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    for (const char *p = "abcdef"; *p; p++) press_char((uint32_t)*p);
    CHECK_EQ(vk_S.len, 3);
    press_sym(PAL_KEY_ENTER);
    CHECK_EQ(g_cb_len, 3);
    CHECK(strcmp(buf, "abc") == 0);
}

TEST(entry_backspace_and_no_eye_key) {
    reset();
    char buf[16] = "";
    pal_vk_entry_t e = {"t", NULL, buf, sizeof(buf), true, true};
    CHECK_EQ(pal_vk_edit_async(&e, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    press_char('a');
    press_char('b');
    press_sym(PAL_KEY_BACKSPACE);
    CHECK_EQ(vk_S.len, 1);
    CHECK_EQ(cell_of_kind(VK_K_SHOW), -1); /* The reference masked entry has no eye key */
    press_sym(PAL_KEY_ESC);
}

TEST(entry_busy_while_open_and_removed_by_others_is_a_cancel) {
    reset();
    char buf[8] = "";
    pal_vk_entry_t e = {"t", NULL, buf, sizeof(buf), false, true};
    g_cb_calls = 0;
    CHECK_EQ(pal_vk_edit_async(&e, entry_cb, NULL), NEMA_OK);
    pal_vk_view.pump(&pal_vk_view);
    pal_vk_entry_t e2 = {"t", NULL, buf, sizeof(buf), false, true};
    CHECK_EQ(pal_vk_edit_async(&e2, entry_cb, NULL), NEMA_ERR_BUSY);
    CHECK_EQ(pal_vk_open(), NEMA_OK); /* queued; the pump refuses to open a second keyboard */
    pal_vk_view.leave(&pal_vk_view);  /* the overlay was removed by the lock or a dialog */
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(g_cb_len, PAL_VK_CANCEL);
}

TEST(blocking_entry_refuses_the_desktop_task_and_no_display) {
    reset();
    char buf[8] = "";
    pal_vk_entry_t e = {"t", NULL, buf, sizeof(buf), false, true};
    /* no display: the caller falls back to its own prompt */
    pal_vk_hooks_t h = HOOKS;
    h.present = present_no;
    pal_vk_set_hooks(&h);
    CHECK_EQ(pal_vk_edit(&e), NEMA_ERR_NOSYS);
    pal_vk_set_hooks(&HOOKS);
    /* the desktop task (the test thread after its pump) must not block */
    pal_vk_view.pump(&pal_vk_view);
    CHECK_EQ(pal_vk_edit(&e), NEMA_ERR_PERM);
    CHECK_EQ(pal_vk_edit(NULL), NEMA_ERR_INVAL);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(every_row_is_twenty_half_units_in_every_layer);
    RUN(layer_key_counts_and_focus_home);
    RUN(left_right_wrap_within_the_row);
    RUN(up_wraps_to_the_bottom_row_nearest_the_column);
    RUN(vertical_moves_keep_the_column);
    RUN(page_keys_jump_to_the_top_and_bottom_rows);
    RUN(shift_latches_then_double_tap_locks);
    RUN(tab_cycles_the_layers_and_entry_skips_ctl);
    RUN(upper_case_follows_shift_and_physical_shift);
    RUN(sticky_ctrl_and_alt_are_applied_once);
    RUN(one_shot_shift_is_used_by_one_letter);
    RUN(key_to_event_translation_table);
    RUN(every_printable_ascii_character_is_on_the_keyboard);
    RUN(text_insert_backspace_and_counting);
    RUN(caret_scrolls_to_stay_visible);
    RUN(box_mirrors_the_app_line_and_keys_reach_the_app);
    RUN(on_screen_keys_send_like_the_physical_keyboard);
    RUN(the_enter_key_sends_enter_and_closes_the_keyboard);
    RUN(fixed_text_does_not_follow_the_app);
    RUN(entry_confirm_returns_the_text);
    RUN(enter_types_the_key_the_arrows_chose_in_an_entry);
    RUN(entry_cancel_leaves_the_buffer);
    RUN(entry_empty_rule);
    RUN(entry_limit_is_cap_minus_one);
    RUN(entry_backspace_and_no_eye_key);
    RUN(entry_busy_while_open_and_removed_by_others_is_a_cancel);
    RUN(blocking_entry_refuses_the_desktop_task_and_no_display);
    return TEST_DONE();
}
