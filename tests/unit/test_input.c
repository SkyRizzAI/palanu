/* The input service (system/src/input.c) on the real hhv2 key table: layers (Nm, Nm+Shift, Fn), the Nm latch,
 * Shift and Ctrl, Alt combinations, the cycle key gesture, key repeat, and the encoding of keys as bytes.
 * Time is driven explicitly through pal_input_tick(now_ms); nothing here reads a clock. The layer data is
 * checked against docs/KEYMAP.md. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/input.h"
#include "test.h"

extern const pal_keyboard_t HHV2_KEYBOARD;

/* Physical position of a key by what it is: a character key by its base letter, a special key by its role. */
static bool at_char(char base, int *r, int *c) {
    const pal_keyboard_t *kb = &HHV2_KEYBOARD;
    for (int i = 0; i < kb->rows * kb->cols; i++) {
        const pal_physkey_t *k = &kb->keys[i];
        if (k->kind == PAL_PK_CHAR && k->base == (uint8_t)base) {
            *r = i / kb->cols, *c = i % kb->cols;
            return true;
        }
    }
    return false;
}
static bool at_kind(uint8_t kind, uint8_t base, int *r, int *c) {
    const pal_keyboard_t *kb = &HHV2_KEYBOARD;
    for (int i = 0; i < kb->rows * kb->cols; i++) {
        const pal_physkey_t *k = &kb->keys[i];
        if (k->kind == kind && (base == 0xFF || k->base == base)) {
            *r = i / kb->cols, *c = i % kb->cols;
            return true;
        }
    }
    return false;
}
static void press_char(char base) {
    int r = -1, c = -1;
    if (!at_char(base, &r, &c)) printf("    no key for '%c'\n", base);
    pal_input_hw(r, c, true);
}
static void release_char(char base) {
    int r = -1, c = -1;
    at_char(base, &r, &c);
    pal_input_hw(r, c, false);
}
static void tap(char base) {
    press_char(base);
    release_char(base);
}
static void hold(uint8_t kind, uint8_t base, bool down) {
    int r = -1, c = -1;
    at_kind(kind, base, &r, &c);
    pal_input_hw(r, c, down);
}
#define SHIFT(d) hold(PAL_PK_SHIFT, 0xFF, (d))
#define NM(d) hold(PAL_PK_NM, 0xFF, (d))
#define FN(d) hold(PAL_PK_FN, 0xFF, (d))
#define CTRL(d) hold(PAL_PK_CTRL, 0xFF, (d))
#define ALT(d) hold(PAL_PK_ALT, 0xFF, (d))
#define CYCLE(d) hold(PAL_PK_CYCLE, 0xFF, (d))

static bool pop(pal_key_t *k) { return pal_input_pop(k); }
static void drain(void) {
    pal_key_t k;
    while (pal_input_pop(&k)) {
    }
}
/* The next event, which must be a character with the given code point and modifiers. */
static bool next_char(uint32_t cp, uint8_t mods) {
    pal_key_t k;
    if (!pop(&k)) return false;
    if (k.sym != PAL_KEY_CHAR || k.cp != cp || k.mods != mods) {
        printf("    got sym %d cp %u (0x%x) mods %u, want char %u mods %u\n", k.sym, k.cp, k.cp, k.mods, cp, mods);
        return false;
    }
    return true;
}
static bool next_sym(uint8_t sym, uint8_t mods) {
    pal_key_t k;
    if (!pop(&k)) return false;
    if (k.sym != sym || k.mods != mods) {
        printf("    got sym %d mods %u, want sym %d mods %u\n", k.sym, k.mods, sym, mods);
        return false;
    }
    return true;
}

static void setup(void) {
    pal_input_init(&HHV2_KEYBOARD);
    pal_input_tick(0);
}

/* ---------------------------------------------------------------- base layer */
TEST(base_layer_letters_space_and_shift) {
    setup();
    tap('a');
    CHECK(next_char('a', 0));
    SHIFT(true);
    tap('a');
    SHIFT(false);
    CHECK(next_char('A', PAL_MOD_SHIFT));
    tap('.');
    CHECK(next_char('.', 0));
    tap(' ');
    CHECK(next_char(' ', 0));
    CHECK(!pal_input_nm_latched());
}

/* ---------------------------------------------------------------- Nm layer, from docs/KEYMAP.md */
static const struct {
    char base;
    uint32_t sym;
} NM_LAYER[] = {
    {'q', '#'}, {'w', '1'}, {'e', '2'}, {'r', '3'}, {'t', '('}, {'y', ')'}, {'u', '_'}, {'i', '-'}, {'o', '+'},
    {'p', '@'}, {'a', '*'}, {'s', '4'}, {'d', '5'}, {'f', '6'}, {'g', '/'}, {'h', ':'}, {'j', ';'}, {'k', '\''},
    {'l', '"'}, {'z', '7'}, {'x', '8'}, {'c', '9'}, {'v', '%'}, {'b', '?'}, {'n', '!'}, {'m', ','}, {'.', '$'},
    {' ', '0'},
};

TEST(nm_hold_gives_the_printed_symbols) {
    setup();
    for (size_t i = 0; i < sizeof(NM_LAYER) / sizeof(NM_LAYER[0]); i++) {
        NM(true);
        tap(NM_LAYER[i].base);
        NM(false);
        if (!next_char(NM_LAYER[i].sym, 0)) CHECK(false);
    }
    drain();
    tap('q');
    CHECK(next_char('q', 0)); /* back to the letters once Nm is released */
}

/* Nm+Shift: the rest of the ASCII set, on the first keys of the top rows (docs/KEYMAP.md). */
static const struct {
    char base;
    uint32_t sym;
} NM_SHIFT_LAYER[] = {
    {'q', '['}, {'w', ']'}, {'e', '{'}, {'r', '}'}, {'t', '<'}, {'y', '>'}, {'u', '='}, {'i', '|'},
    {'o', '\\'}, {'p', '~'}, {'a', '&'}, {'s', '^'}, {'d', '`'},
};

TEST(nm_shift_gives_the_secondary_symbols) {
    setup();
    for (size_t i = 0; i < sizeof(NM_SHIFT_LAYER) / sizeof(NM_SHIFT_LAYER[0]); i++) {
        NM(true);
        SHIFT(true);
        tap(NM_SHIFT_LAYER[i].base);
        SHIFT(false);
        NM(false);
        /* the symbol is already resolved; the Shift that selected the layer is still reported in mods */
        if (!next_char(NM_SHIFT_LAYER[i].sym, PAL_MOD_SHIFT)) CHECK(false);
    }
}

TEST(nm_latch_applies_to_one_character) {
    setup();
    NM(true);
    NM(false); /* a tap */
    CHECK(pal_input_nm_latched());
    tap('q');
    CHECK(next_char('#', 0));
    CHECK(!pal_input_nm_latched());
    tap('w');
    CHECK(next_char('w', 0)); /* the latch was used up */
}

TEST(nm_used_while_held_does_not_latch) {
    setup();
    NM(true);
    tap('q');
    NM(false);
    CHECK(next_char('#', 0));
    CHECK(!pal_input_nm_latched());
    tap('w');
    CHECK(next_char('w', 0));
}

/* ---------------------------------------------------------------- Fn navigation */
static const struct {
    char base;
    uint8_t sym;
} FN_LAYER[] = {
    {'w', PAL_KEY_UP}, {'a', PAL_KEY_LEFT}, {'s', PAL_KEY_DOWN}, {'d', PAL_KEY_RIGHT},
    {'q', PAL_KEY_HOME}, {'e', PAL_KEY_END}, {'r', PAL_KEY_PGUP}, {'f', PAL_KEY_PGDN},
};

TEST(fn_layer_navigation) {
    setup();
    for (size_t i = 0; i < sizeof(FN_LAYER) / sizeof(FN_LAYER[0]); i++) {
        FN(true);
        tap(FN_LAYER[i].base);
        FN(false);
        if (!next_sym(FN_LAYER[i].sym, 0)) CHECK(false);
    }
    FN(true);
    int r = -1, c = -1;
    at_kind(PAL_PK_SPECIAL, PAL_KEY_BACKSPACE, &r, &c); /* the "del" key */
    pal_input_hw(r, c, true);
    pal_input_hw(r, c, false);
    FN(false);
    CHECK(next_sym(PAL_KEY_DELETE, 0));
    tap('w');
    CHECK(next_char('w', 0)); /* without Fn the key is a letter */
}

TEST(shift_fn_r_scrolls_history) {
    setup();
    SHIFT(true);
    FN(true);
    tap('r');
    FN(false);
    SHIFT(false);
    CHECK(next_sym(PAL_KEY_PGUP, PAL_MOD_SHIFT)); /* Shift + PgUp (Fn+R) is what scrolls the terminal */
}

/* ---------------------------------------------------------------- Alt, Ctrl, special keys */
TEST(alt_and_ctrl_combinations) {
    setup();
    ALT(true);
    tap('n');
    ALT(false);
    CHECK(next_char('n', PAL_MOD_ALT));
    ALT(true);
    tap('w');
    ALT(false);
    CHECK(next_char('w', PAL_MOD_ALT));
    ALT(true);
    int r = -1, c = -1;
    at_kind(PAL_PK_SPECIAL, PAL_KEY_TAB, &r, &c);
    pal_input_hw(r, c, true);
    pal_input_hw(r, c, false);
    ALT(false);
    CHECK(next_sym(PAL_KEY_TAB, PAL_MOD_ALT));
    CTRL(true);
    tap('c');
    CTRL(false);
    CHECK(next_char('c', PAL_MOD_CTRL));
}

TEST(special_keys) {
    setup();
    int r = -1, c = -1;
    at_kind(PAL_PK_SPECIAL, PAL_KEY_ENTER, &r, &c);
    pal_input_hw(r, c, true);
    pal_input_hw(r, c, false);
    CHECK(next_sym(PAL_KEY_ENTER, 0));
    at_kind(PAL_PK_SPECIAL, PAL_KEY_ESC, &r, &c);
    pal_input_hw(r, c, true);
    pal_input_hw(r, c, false);
    CHECK(next_sym(PAL_KEY_ESC, 0));
}

/* ---------------------------------------------------------------- cycle key: tap switches, hold goes home */
TEST(cycle_tap_switches_app) {
    setup();
    pal_input_tick(100);
    CYCLE(true);
    pal_input_tick(250);
    CYCLE(false);
    CHECK(next_sym(PAL_KEY_SWITCH, 0));
    pal_key_t k;
    CHECK(!pop(&k));
}

TEST(cycle_hold_goes_home_at_550ms) {
    setup();
    pal_input_tick(1000);
    CYCLE(true);
    CHECK_EQ(pal_input_next_tick_ms(1000), 550);
    pal_input_tick(1549);
    pal_key_t k;
    CHECK(!pop(&k));
    pal_input_tick(1550);
    CHECK(next_sym(PAL_KEY_GOHOME, 0));
    CYCLE(false);
    pal_input_tick(1600);
    CHECK(!pop(&k)); /* a released hold is not also a switch */
    CHECK_EQ(pal_input_next_tick_ms(1600), -1);
}

/* ---------------------------------------------------------------- a press after the desktop slept */
/* The keyboard task reports keys between desktop ticks, and the idle desktop sleeps up to a second. A press must be
 * stamped with the time it happened, not with the time of the last tick: otherwise the first repeat is already due and
 * one short tap types the letter several times, and a short tap of the cycle key counts as a long press. */
static uint32_t g_clock;
static uint32_t fake_clock(void) { return g_clock; }

TEST(a_short_tap_after_an_idle_second_types_the_letter_once) {
    setup();
    pal_input_set_clock(fake_clock);
    pal_input_tick(0); /* the last desktop tick ... */
    g_clock = 1000;    /* ... and the desktop has slept for a second since */
    press_char('h');
    pal_input_tick(1005); /* the desktop wakes because of the key */
    g_clock = 1080;
    release_char('h');
    pal_input_tick(1085);
    pal_input_tick(1200);
    CHECK(next_char('h', 0));
    pal_key_t k;
    CHECK(!pop(&k)); /* no repeat of a tap that lasted 80 ms */
    pal_input_set_clock(NULL);
}

TEST(a_held_key_still_repeats_420ms_after_the_real_press) {
    setup();
    pal_input_set_clock(fake_clock);
    pal_input_tick(0);
    g_clock = 1000;
    press_char('h');
    pal_input_tick(1005);
    pal_input_tick(1419);
    CHECK(next_char('h', 0));
    pal_key_t k;
    CHECK(!pop(&k));
    pal_input_tick(1421);
    CHECK(pop(&k) && k.repeat);
    pal_input_set_clock(NULL);
}

TEST(a_short_cycle_tap_after_an_idle_second_is_not_a_long_press) {
    setup();
    pal_input_set_clock(fake_clock);
    pal_input_tick(0);
    g_clock = 1000;
    CYCLE(true);
    pal_input_tick(1010); /* the desktop wakes: the press is 10 ms old, not 1010 ms */
    g_clock = 1120;
    CYCLE(false);
    CHECK(next_sym(PAL_KEY_SWITCH, 0)); /* a tap: switch, not home */
    pal_key_t k;
    CHECK(!pop(&k));
    pal_input_set_clock(NULL);
}

/* ---------------------------------------------------------------- key repeat */
TEST(repeat_after_420ms_then_every_55ms) {
    setup();
    pal_input_tick(0);
    tap('a'); /* press and release at once: a plain letter, then nothing */
    drain();
    press_char('a');
    CHECK(next_char('a', 0));
    CHECK_EQ(pal_input_next_tick_ms(0), 420);
    pal_input_tick(419);
    pal_key_t k;
    CHECK(!pop(&k));
    pal_input_tick(420);
    CHECK(pop(&k) && k.repeat && k.cp == 'a');
    CHECK_EQ(pal_input_next_tick_ms(420), 55);
    pal_input_tick(474);
    CHECK(!pop(&k));
    pal_input_tick(475);
    CHECK(pop(&k) && k.repeat);
    release_char('a');
    pal_input_tick(2000);
    CHECK(!pop(&k));
    CHECK_EQ(pal_input_next_tick_ms(2000), -1);
}

TEST(modifiers_do_not_repeat) {
    setup();
    SHIFT(true);
    pal_input_tick(2000);
    pal_key_t k;
    CHECK(!pop(&k));
    CHECK_EQ(pal_input_next_tick_ms(2000), -1);
    SHIFT(false);
}

TEST(new_key_replaces_the_repeat) {
    setup();
    pal_input_tick(0);
    press_char('a');
    drain();
    pal_input_tick(300);
    press_char('b');
    drain();
    /* b is the newest key: it repeats 420 ms after its own press (a is still held and no longer repeats) */
    pal_input_tick(719);
    pal_key_t k;
    CHECK(!pop(&k));
    pal_input_tick(720);
    CHECK(pop(&k) && k.repeat && k.cp == 'b');
    release_char('b');
    release_char('a');
}

TEST(releasing_another_key_keeps_the_repeat) {
    setup();
    pal_input_tick(0);
    press_char('a');
    drain();
    press_char('b');
    drain();
    release_char('a'); /* a was replaced by b; releasing it must not cancel b's repeat */
    pal_input_tick(420);
    pal_key_t k;
    CHECK(pop(&k) && k.repeat && k.cp == 'b');
    release_char('b');
}

/* ---------------------------------------------------------------- queue */
TEST(queue_is_bounded_and_keeps_order) {
    setup();
    for (uint32_t i = 0; i < 40; i++) {
        pal_key_t k = {PAL_KEY_CHAR, 0, false, 'A' + (i % 26)};
        pal_input_inject(&k);
    }
    nema_obj_t *o = pal_input_obj();
    CHECK(o->ops->poll(o, NEMA_POLL_IN) != 0);
    int n = 0;
    pal_key_t k;
    uint32_t first = 0;
    while (pop(&k)) {
        if (n == 0) first = k.cp;
        n++;
    }
    CHECK_EQ(n, 32);
    CHECK_EQ(first, 'A');
    CHECK(o->ops->poll(o, NEMA_POLL_IN) == 0);
}

/* ---------------------------------------------------------------- key bytes for the terminal */
TEST(key_to_bytes_encoding) {
    char b[16];
    pal_key_t up = {PAL_KEY_UP, 0, false, 0};
    CHECK_EQ(pal_key_to_bytes(&up, false, b, sizeof(b)), 3);
    CHECK(memcmp(b, "\x1b[A", 3) == 0);
    CHECK_EQ(pal_key_to_bytes(&up, true, b, sizeof(b)), 3);
    CHECK(memcmp(b, "\x1bOA", 3) == 0); /* application cursor keys */
    pal_key_t enter = {PAL_KEY_ENTER, 0, false, 0};
    CHECK_EQ(pal_key_to_bytes(&enter, false, b, sizeof(b)), 1);
    CHECK(b[0] == '\r');
    pal_key_t pgup = {PAL_KEY_PGUP, 0, false, 0};
    CHECK_EQ(pal_key_to_bytes(&pgup, false, b, sizeof(b)), 4);
    CHECK(memcmp(b, "\x1b[5~", 4) == 0);
    pal_key_t alt_x = {PAL_KEY_CHAR, PAL_MOD_ALT, false, 'x'};
    CHECK_EQ(pal_key_to_bytes(&alt_x, false, b, sizeof(b)), 2);
    CHECK(memcmp(b, "\x1bx", 2) == 0);
    pal_key_t ctrl_c = {PAL_KEY_CHAR, PAL_MOD_CTRL, false, 'c'};
    CHECK_EQ(pal_key_to_bytes(&ctrl_c, false, b, sizeof(b)), 1);
    CHECK(b[0] == 0x03);
    pal_key_t ctrl_q = {PAL_KEY_CHAR, PAL_MOD_CTRL, false, '?'};
    CHECK_EQ(pal_key_to_bytes(&ctrl_q, false, b, sizeof(b)), 1);
    CHECK(b[0] == 0x7f);
    pal_key_t euro = {PAL_KEY_CHAR, 0, false, 0x20AC};
    CHECK_EQ(pal_key_to_bytes(&euro, false, b, sizeof(b)), 3);
    pal_key_t sw = {PAL_KEY_SWITCH, 0, false, 0};
    CHECK_EQ(pal_key_to_bytes(&sw, false, b, sizeof(b)), 0); /* not a byte sequence */
}

/* ---------------------------------------------------------------- Fn double tap (PAL_KEY_VKB, vk.h) */
static int vkb_count(void) {
    pal_key_t k;
    int n = 0;
    while (pal_input_pop(&k))
        if (k.sym == PAL_KEY_VKB) n++;
    return n;
}
/* One tap of Fn: down at down_ms, up at up_ms (the fake clock is the time of each event). */
static void fn_tap(uint32_t down_ms, uint32_t up_ms) {
    g_clock = down_ms;
    FN(true);
    g_clock = up_ms;
    FN(false);
}
static void vkb_setup(void) {
    setup();
    pal_input_set_clock(fake_clock);
    g_clock = 1000;
}

TEST(fn_double_tap_emits_one_vkb) {
    vkb_setup();
    fn_tap(1000, 1100);
    CHECK_EQ(vkb_count(), 0); /* one tap alone does nothing */
    fn_tap(1200, 1300);       /* 100 ms gap */
    CHECK_EQ(vkb_count(), 1);
    pal_input_set_clock(NULL);
}

TEST(fn_with_another_key_is_not_a_tap) {
    vkb_setup();
    g_clock = 1000;
    FN(true);
    g_clock = 1050;
    tap('w'); /* Fn+W: the arrow up, and Fn is no longer a clean tap */
    g_clock = 1100;
    FN(false);
    CHECK(next_sym(PAL_KEY_UP, 0));
    CHECK_EQ(vkb_count(), 0);
    fn_tap(1200, 1300); /* the second Fn tap alone: no VKB, because the first one did not count */
    CHECK_EQ(vkb_count(), 0);
    pal_input_set_clock(NULL);
}

TEST(slow_or_long_fn_taps_do_not_open) {
    vkb_setup();
    fn_tap(1000, 1100);
    fn_tap(1500, 1600); /* gap of 400 ms */
    CHECK_EQ(vkb_count(), 0);
    fn_tap(2000, 2400); /* a press of 400 ms is a hold, not a tap */
    fn_tap(2500, 2600);
    CHECK_EQ(vkb_count(), 0);
    pal_input_set_clock(NULL);
}

TEST(triple_tap_opens_once) {
    vkb_setup();
    fn_tap(1000, 1100);
    fn_tap(1200, 1300);
    fn_tap(1400, 1500); /* the third tap starts a new count */
    CHECK_EQ(vkb_count(), 1);
    pal_input_set_clock(NULL);
}

TEST(key_repeat_is_unaffected_by_the_gesture) {
    vkb_setup();
    press_char('a'); /* held: repeats after 420 ms, then every 55 ms */
    for (uint32_t t = 1000; t <= 2000; t += 20) {
        g_clock = t;
        pal_input_tick(t);
    }
    release_char('a');
    int a = 0;
    pal_key_t k;
    while (pal_input_pop(&k))
        if (k.sym == PAL_KEY_CHAR && k.cp == 'a') a++;
    CHECK(a >= 10);
    CHECK_EQ(vkb_count(), 0);
    pal_input_set_clock(NULL);
}

TEST(modifier_query_reports_held_keys) {
    setup();
    CHECK_EQ(pal_input_mods_held(), 0);
    SHIFT(true);
    CHECK(pal_input_mods_held() & PAL_MOD_SHIFT);
    CTRL(true);
    CHECK(pal_input_mods_held() & PAL_MOD_CTRL);
    SHIFT(false);
    CTRL(false);
    CHECK_EQ(pal_input_mods_held(), 0);
}

/* ---------------------------------------------------------------- sticky modes: Caps, Symbols, Numbers, Ctrl */
static void shift_tap(uint32_t down_ms, uint32_t up_ms) {
    g_clock = down_ms;
    SHIFT(true);
    g_clock = up_ms;
    SHIFT(false);
}

TEST(shift_tapped_twice_toggles_caps_lock) {
    vkb_setup();
    shift_tap(1000, 1100);
    CHECK(!pal_input_mode(PAL_INPUT_CAPS)); /* one tap alone does nothing */
    shift_tap(1200, 1300);
    CHECK(pal_input_mode(PAL_INPUT_CAPS));
    tap('a');
    CHECK(next_char('A', 0)); /* upper case without Shift held, and no Shift modifier on the key */
    tap('.');
    CHECK(next_char('.', 0));
    SHIFT(true);
    tap('a');
    SHIFT(false);
    CHECK(next_char('a', PAL_MOD_SHIFT)); /* Shift held with Caps on gives the lower case letter */
    shift_tap(2000, 2100);
    shift_tap(2200, 2300);
    CHECK(!pal_input_mode(PAL_INPUT_CAPS));
    tap('a');
    CHECK(next_char('a', 0));
    pal_input_set_clock(NULL);
}

TEST(shift_with_a_letter_or_slow_taps_is_not_caps) {
    vkb_setup();
    g_clock = 1000;
    SHIFT(true);
    tap('a');
    g_clock = 1100;
    SHIFT(false);
    shift_tap(1200, 1300); /* the first tap does not count: another key went down while Shift was held */
    CHECK(!pal_input_mode(PAL_INPUT_CAPS));
    drain();
    shift_tap(3000, 3600); /* a long press */
    shift_tap(3700, 3800);
    CHECK(!pal_input_mode(PAL_INPUT_CAPS));
    shift_tap(5000, 5100);
    shift_tap(6000, 6100); /* too far apart */
    CHECK(!pal_input_mode(PAL_INPUT_CAPS));
    pal_input_set_clock(NULL);
}

TEST(symbols_lock_keeps_the_nm_layer_and_nm_flips_it_back) {
    setup();
    pal_input_set_mode(PAL_INPUT_SYM, true);
    tap('q');
    CHECK(next_char('#', 0));
    tap(' ');
    CHECK(next_char('0', 0));
    NM(true);
    tap('q');
    NM(false);
    CHECK(next_char('q', 0)); /* holding Nm while locked gives the letters back */
    tap('q');
    CHECK(next_char('#', 0));
    pal_input_set_mode(PAL_INPUT_SYM, false);
    tap('q');
    CHECK(next_char('q', 0));
}

TEST(numbers_lock_changes_only_the_digit_keys) {
    setup();
    pal_input_set_mode(PAL_INPUT_NUM, true);
    tap('w');
    CHECK(next_char('1', 0));
    tap('c');
    CHECK(next_char('9', 0));
    tap(' ');
    CHECK(next_char('0', 0));
    tap('q'); /* not a digit key: stays a letter */
    CHECK(next_char('q', 0));
    NM(true);
    tap('q');
    NM(false);
    CHECK(next_char('#', 0)); /* the full symbol layer is still one hold away */
    pal_input_set_mode(PAL_INPUT_SYM, true); /* the two locks exclude each other */
    CHECK(!pal_input_mode(PAL_INPUT_NUM));
    pal_input_set_mode(PAL_INPUT_NUM, true);
    CHECK(!pal_input_mode(PAL_INPUT_SYM));
}

TEST(ctrl_applies_to_the_next_key_only_and_does_not_repeat) {
    setup();
    pal_input_set_mode(PAL_INPUT_CTRL, true);
    tap('c');
    CHECK(next_char('c', PAL_MOD_CTRL));
    CHECK(!pal_input_mode(PAL_INPUT_CTRL));
    tap('c');
    CHECK(next_char('c', 0));
    pal_input_set_mode(PAL_INPUT_CTRL, true);
    press_char('c');
    pal_input_tick(1500); /* a held key would repeat: this one is a single Ctrl+C */
    release_char('c');
    CHECK(next_char('c', PAL_MOD_CTRL));
    pal_key_t k;
    CHECK(!pop(&k));
}

/* One tap of Fn is reported (PAL_KEY_FNTAP) only while asked, and the second tap of a pair is still PAL_KEY_VKB. */
static void count_syms(int *fntap, int *vkb) {
    pal_key_t k;
    *fntap = *vkb = 0;
    while (pal_input_pop(&k)) {
        if (k.sym == PAL_KEY_FNTAP) (*fntap)++;
        if (k.sym == PAL_KEY_VKB) (*vkb)++;
    }
}

TEST(fn_tap_is_reported_only_when_asked) {
    vkb_setup();
    int t, v;
    fn_tap(1000, 1100);
    count_syms(&t, &v);
    CHECK_EQ(t, 0); /* not asked: nothing but the double tap is sent */
    pal_input_report_fn_tap(true);
    fn_tap(3000, 3100);
    count_syms(&t, &v);
    CHECK_EQ(t, 1);
    CHECK_EQ(v, 0);
    fn_tap(5000, 5100); /* a quick pair: the first tap is a tap, the second is the double tap */
    fn_tap(5200, 5300);
    count_syms(&t, &v);
    CHECK_EQ(t, 1);
    CHECK_EQ(v, 1);
    fn_tap(7000, 7600); /* a long press is not a tap */
    count_syms(&t, &v);
    CHECK_EQ(t, 0);
    g_clock = 9000;
    FN(true);
    g_clock = 9050;
    tap('w'); /* Fn+W is an arrow, not a tap of Fn */
    g_clock = 9100;
    FN(false);
    drain();
    fn_tap(11000, 11100);
    count_syms(&t, &v);
    CHECK_EQ(t, 1);
    pal_input_report_fn_tap(false);
    fn_tap(13000, 13100);
    count_syms(&t, &v);
    CHECK_EQ(t, 0);
    pal_input_set_clock(NULL);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(base_layer_letters_space_and_shift);
    RUN(nm_hold_gives_the_printed_symbols);
    RUN(nm_shift_gives_the_secondary_symbols);
    RUN(nm_latch_applies_to_one_character);
    RUN(nm_used_while_held_does_not_latch);
    RUN(fn_layer_navigation);
    RUN(shift_fn_r_scrolls_history);
    RUN(alt_and_ctrl_combinations);
    RUN(special_keys);
    RUN(cycle_tap_switches_app);
    RUN(cycle_hold_goes_home_at_550ms);
    RUN(a_short_tap_after_an_idle_second_types_the_letter_once);
    RUN(a_held_key_still_repeats_420ms_after_the_real_press);
    RUN(a_short_cycle_tap_after_an_idle_second_is_not_a_long_press);
    RUN(repeat_after_420ms_then_every_55ms);
    RUN(modifiers_do_not_repeat);
    RUN(new_key_replaces_the_repeat);
    RUN(releasing_another_key_keeps_the_repeat);
    RUN(queue_is_bounded_and_keeps_order);
    RUN(key_to_bytes_encoding);
    RUN(fn_double_tap_emits_one_vkb);
    RUN(fn_with_another_key_is_not_a_tap);
    RUN(slow_or_long_fn_taps_do_not_open);
    RUN(triple_tap_opens_once);
    RUN(fn_tap_is_reported_only_when_asked);
    RUN(shift_tapped_twice_toggles_caps_lock);
    RUN(shift_with_a_letter_or_slow_taps_is_not_caps);
    RUN(symbols_lock_keeps_the_nm_layer_and_nm_flips_it_back);
    RUN(numbers_lock_changes_only_the_digit_keys);
    RUN(ctrl_applies_to_the_next_key_only_and_does_not_repeat);
    RUN(key_repeat_is_unaffected_by_the_gesture);
    RUN(modifier_query_reports_held_keys);
    return TEST_DONE();
}
