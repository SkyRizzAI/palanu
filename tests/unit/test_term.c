#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/font.h"
#include "pal/term.h"
#include "pal/theme.h"
#include "test.h"

static pal_term_t *T;

static void W(const char *s) { pal_term_write(T, s, strlen(s)); }
static void fresh(int cols, int rows) {
    pal_term_destroy(T);
    T = pal_term_create(cols, rows, 50, NEMA_OWNER_KERNEL);
}
static bool row_is(int row, const char *want) {
    char b[256];
    pal_term_row_text(T, row, b, sizeof(b));
    if (strcmp(b, want) != 0) printf("    row %d: got \"%s\" want \"%s\"\n", row, b, want);
    return strcmp(b, want) == 0;
}
#define ROW(r, s) CHECK(row_is((r), (s)))
static pal_cell_t cell(int c, int r) { return *pal_term_cell(T, c, r); }

TEST(text_and_newlines) {
    fresh(20, 5);
    W("hello\r\nworld");
    ROW(0, "hello");
    ROW(1, "world");
    int c, r;
    bool v;
    pal_term_cursor(T, &c, &r, &v);
    CHECK_EQ(c, 5);
    CHECK_EQ(r, 1);
    CHECK(v);
}

TEST(autowrap_uses_pending_wrap) {
    fresh(10, 4);
    W("0123456789");
    int c, r;
    bool v;
    pal_term_cursor(T, &c, &r, &v);
    CHECK_EQ(c, 9); /* stays on the last column until the next character */
    CHECK_EQ(r, 0);
    W("ab");
    ROW(0, "0123456789");
    ROW(1, "ab");
}

TEST(cursor_addressing_and_erase) {
    fresh(20, 6);
    W("\x1b[3;5Hx");
    ROW(2, "    x");
    W("\x1b[Hab\x1b[2;1Hcd");
    ROW(0, "ab");
    ROW(1, "cd");
    W("\x1b[1;2H\x1b[K"); /* erase from cursor to end of line */
    ROW(0, "a");
    W("\x1b[2J");
    ROW(1, "");
    ROW(2, "");
    W("\x1b[3;1Habc\x1b[1K"); /* erase from line start to cursor */
    ROW(2, "");
}

TEST(scrolling_feeds_scrollback) {
    fresh(10, 3);
    W("1\r\n2\r\n3\r\n4\r\n5");
    ROW(0, "3");
    ROW(1, "4");
    ROW(2, "5");
    CHECK_EQ(pal_term_scrollback_used(T), 2);
    pal_term_scroll_view(T, 2);
    ROW(0, "1");
    ROW(1, "2");
    ROW(2, "3");
    pal_term_scroll_view(T, -5);
    ROW(0, "3");
}

TEST(sgr_colours_and_attributes) {
    fresh(20, 3);
    W("\x1b[1;31mR\x1b[0m\x1b[38;5;200mP\x1b[48;2;255;0;0mB\x1b[0m\x1b[4;7mU\x1b[0mN");
    CHECK_EQ(cell(0, 0).fg, 1);
    CHECK(cell(0, 0).attr & PAL_TERM_ATTR_BOLD);
    CHECK_EQ(cell(1, 0).fg, 200);
    CHECK_EQ(cell(2, 0).bg, 196); /* pure red lands on the 6x6x6 cube corner */
    CHECK((cell(3, 0).attr & (PAL_TERM_ATTR_UNDERLINE | PAL_TERM_ATTR_REVERSE)) ==
          (PAL_TERM_ATTR_UNDERLINE | PAL_TERM_ATTR_REVERSE));
    CHECK_EQ(cell(4, 0).attr, 0);
    CHECK_EQ(cell(4, 0).fg, PAL_C_TERM_FG);
    W("\x1b[91;102mx");
    CHECK_EQ(cell(5, 0).fg, 9);
    CHECK_EQ(cell(5, 0).bg, 10);
}

TEST(alternate_screen_restores_main) {
    fresh(20, 4);
    W("main text");
    W("\x1b[?1049h\x1b[H\x1b[2Jalt");
    CHECK(pal_term_alt_screen(T));
    ROW(0, "alt");
    W("\x1b[?1049l");
    CHECK(!pal_term_alt_screen(T));
    ROW(0, "main text");
    int c, r;
    bool v;
    pal_term_cursor(T, &c, &r, &v);
    CHECK_EQ(c, 9); /* cursor restored to where it was before the switch */
}

TEST(scroll_region_like_an_editor) {
    fresh(10, 6);
    W("\x1b[1;1Htop\r\n1\r\n2\r\n3\r\n4\r\nbottom");
    /* an editor pins the status line (row 6) and scrolls rows 1..5 */
    W("\x1b[1;5r\x1b[5;1H\n");
    ROW(0, "1");
    ROW(3, "4");
    ROW(4, "");
    ROW(5, "bottom"); /* outside the region: untouched */
    CHECK_EQ(pal_term_scrollback_used(T), 0); /* region scrolls do not feed history */
    W("\x1b[1;1H\x1bM"); /* reverse index at the top of the region scrolls down */
    ROW(0, "");
    ROW(1, "1");
    W("\x1b[r");
}

TEST(insert_and_delete) {
    fresh(10, 5);
    W("abcdef\x1b[1;3H\x1b[2@");
    ROW(0, "ab  cdef");
    W("\x1b[1;3H\x1b[3P");
    ROW(0, "abdef");
    W("\x1b[1;1H\x1b[2L");
    ROW(0, "");
    ROW(2, "abdef");
    W("\x1b[1M");
    ROW(1, "abdef");
    W("\x1b[1;1H\x1b[3X");
}

TEST(tabs) {
    fresh(30, 3);
    W("a\tb\tc");
    ROW(0, "a       b       c");
    W("\r\x1b[3g\tX");
    W("\x1bH"); /* set a stop at the cursor */
}

TEST(utf8_and_line_drawing) {
    fresh(20, 3);
    W("caf\xC3\xA9 \xE2\x94\x80\xE2\x94\x82");
    ROW(0, "caf\xC3\xA9 \xE2\x94\x80\xE2\x94\x82");
    W("\r\n\x1b(0lqk\x1b(B lqk");
    ROW(1, "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90 lqk");
}

static char g_reply[64];
static void on_reply(void *ctx, const char *b, size_t n) {
    (void)ctx;
    size_t m = strlen(g_reply);
    if (m + n < sizeof(g_reply)) memcpy(g_reply + m, b, n), g_reply[m + n] = 0;
}

TEST(device_status_replies) {
    fresh(20, 8);
    pal_term_set_reply(T, on_reply, NULL);
    g_reply[0] = 0;
    W("\x1b[4;7H\x1b[6n");
    CHECK(strcmp(g_reply, "\x1b[4;7R") == 0);
    g_reply[0] = 0;
    W("\x1b[c");
    CHECK(strncmp(g_reply, "\x1b[?", 3) == 0);
    g_reply[0] = 0;
    W("\x1b[5n");
    CHECK(strcmp(g_reply, "\x1b[0n") == 0);
}

TEST(modes_are_tracked) {
    fresh(20, 3);
    CHECK(!pal_term_app_cursor_keys(T));
    W("\x1b[?1h\x1b[?2004h\x1b[?25l");
    CHECK(pal_term_app_cursor_keys(T));
    CHECK(pal_term_bracketed_paste(T));
    int c, r;
    bool v;
    pal_term_cursor(T, &c, &r, &v);
    CHECK(!v);
    W("\x1b[?1l\x1b[?2004l\x1b[?25h");
    CHECK(!pal_term_app_cursor_keys(T));
}

TEST(sequences_split_across_writes) {
    fresh(20, 3);
    const char *seq = "\x1b[1;31mred\x1b[0m \xE2\x94\x80 \x1b[2;3Hz";
    for (size_t i = 0; seq[i]; i++) pal_term_write(T, &seq[i], 1);
    CHECK_EQ(cell(0, 0).fg, 1);
    ROW(0, "red \xE2\x94\x80");
    ROW(1, "  z");
}

TEST(strings_are_swallowed) {
    fresh(20, 3);
    W("a\x1b]0;window title\x07" "b\x1bPdcs data\x1b\\" "c\x1b_apc\x1b\\" "d");
    ROW(0, "abcd");
}

TEST(resize_keeps_content) {
    fresh(20, 5);
    W("one\r\ntwo\r\nthree");
    pal_term_resize(T, 30, 8);
    ROW(0, "one");
    ROW(2, "three");
    pal_term_resize(T, 10, 2); /* shrink: the cursor line must stay visible */
    ROW(1, "three");
    CHECK_EQ(pal_term_cols(T), 10);
}

TEST(drawing_honours_colours_and_cursor) {
    fresh(4, 2);
    W("\x1b[31mA");
    static uint8_t px[64 * 32];
    pal_canvas_t cv;
    pal_canvas_init(&cv, px, 64, 32);
    pal_term_mark_all(T);
    int drawn = pal_term_draw(T, &cv, &PAL_FONT_SPLEEN_8X16, 0, 0, true);
    CHECK_EQ(drawn, 2);
    bool any_red = false, any_bg = false;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++) {
            if (pal_get(&cv, x, y) == 1) any_red = true;
            if (pal_get(&cv, x, y) == PAL_C_TERM_BG) any_bg = true;
        }
    CHECK(any_red);
    CHECK(any_bg);
    CHECK_EQ(pal_term_draw(T, &cv, &PAL_FONT_SPLEEN_8X16, 0, 0, true), 0); /* nothing dirty now */
    W("x");
    CHECK_EQ(pal_term_draw(T, &cv, &PAL_FONT_SPLEEN_8X16, 0, 0, true), 1); /* only the changed row */
}

TEST(full_screen_program_session) {
    /* the shape of a vim start-up and a few edits: alternate screen, region, status line, cursor motion */
    fresh(40, 12);
    W("$ vim f\r\n");
    W("\x1b[?1049h\x1b[22;0;0t\x1b[?1h\x1b=\x1b[?2004h\x1b[H\x1b[2J");
    for (int i = 1; i <= 10; i++) W("~\r\n");
    W("\x1b[12;1H\x1b[7m\"f\" [New]   0,0-1  All\x1b[0m\x1b[1;1H");
    W("hello\x1b[1;3H\x1b[3Pxy"); /* delete chars and overtype */
    ROW(0, "hexy");
    ROW(1, "~");
    ROW(11, "\"f\" [New]   0,0-1  All");
    CHECK(cell(0, 11).attr & PAL_TERM_ATTR_REVERSE);
    W("\x1b[?2004l\x1b[?1l\x1b>\x1b[?1049l");
    ROW(0, "$ vim f");
}

TEST(history_is_freed_under_pressure) {
    fresh(20, 5);
    for (int i = 0; i < 60; i++) W("line\r\n");
    CHECK(pal_term_scrollback_used(T) > 0);
    size_t freed = pal_term_trim_history();
    CHECK(freed > 0);
    CHECK_EQ(pal_term_scrollback_used(T), 0);
    W("after\r\n");
    ROW(3, "after"); /* the screen still works, and a trimmed terminal keeps no history */
    CHECK_EQ(pal_term_scrollback_used(T), 0);
    /* the reclaimer is what a refused allocation calls: an impossible request must free history on the way */
    fresh(20, 5);
    for (int i = 0; i < 60; i++) W("again\r\n");
    CHECK(pal_term_scrollback_used(T) > 0);
    CHECK(nema_alloc(NEMA_MEM_EXTERNAL, NEMA_OWNER_KERNEL, 16u * 1024u * 1024u - 1024u) == NULL);
    CHECK_EQ(pal_term_scrollback_used(T), 0);
}

TEST(wide_characters_take_two_columns) {
    pal_term_t *t = pal_term_create(10, 3, 0, NEMA_OWNER_KERNEL);
    const char *s = "a\xe4\xbd\xa0" "b\xf0\x9f\x9a\x80" "c"; /* a 你 b 🚀 c */
    pal_term_write(t, s, strlen(s));
    int cx, cy;
    bool vis;
    pal_term_cursor(t, &cx, &cy, &vis);
    CHECK_EQ(cx, 7); /* 1 + 2 + 1 + 2 + 1 */
    CHECK(pal_term_cell(t, 1, 0)->attr & PAL_TERM_ATTR_WIDE);
    CHECK(pal_term_cell(t, 2, 0)->attr & PAL_TERM_ATTR_WIDE_CONT);
    CHECK_EQ(pal_term_cell(t, 4, 0)->ch, 0x1F680u);
    char line[64];
    pal_term_row_text(t, 0, line, sizeof(line));
    CHECK(strcmp(line, s) == 0); /* the text reads back whole, the second halves skipped */
    pal_term_destroy(t);
}

TEST(combining_marks_take_no_column) {
    pal_term_t *t = pal_term_create(10, 3, 0, NEMA_OWNER_KERNEL);
    const char *s = "e\xcc\x81x\xe2\x9c\x85\xef\xb8\x8fy"; /* e + combining acute, x, ✅ + VS16, y */
    pal_term_write(t, s, strlen(s));
    int cx, cy;
    bool vis;
    pal_term_cursor(t, &cx, &cy, &vis);
    CHECK_EQ(cx, 5); /* e(1) x(1) ✅(2) y(1) */
    pal_term_destroy(t);
}

TEST(wide_character_in_the_last_column_wraps_first) {
    pal_term_t *t = pal_term_create(5, 3, 0, NEMA_OWNER_KERNEL);
    const char *s = "abcd\xe4\xbd\xa0"; /* 4 columns, then a wide one that cannot fit in the fifth */
    pal_term_write(t, s, strlen(s));
    CHECK_EQ(pal_term_cell(t, 4, 0)->ch, (uint32_t)' ');
    CHECK_EQ(pal_term_cell(t, 0, 1)->ch, 0x4F60u);
    CHECK(pal_term_cell(t, 1, 1)->attr & PAL_TERM_ATTR_WIDE_CONT);
    pal_term_destroy(t);
}

TEST(overwriting_half_a_wide_character_blanks_the_other_half) {
    pal_term_t *t = pal_term_create(10, 3, 0, NEMA_OWNER_KERNEL);
    const char *s = "\xe4\xbd\xa0\x1b[1;2Hz"; /* 你 at columns 1-2, then z over its second half */
    pal_term_write(t, s, strlen(s));
    CHECK_EQ(pal_term_cell(t, 0, 0)->ch, (uint32_t)' ');
    CHECK(!(pal_term_cell(t, 0, 0)->attr & PAL_TERM_ATTR_WIDE));
    CHECK_EQ(pal_term_cell(t, 1, 0)->ch, (uint32_t)'z');
    pal_term_destroy(t);
}

/* The opencode installer redraws a 50-cell bar of ■ and ･ (U+FF65, one column) with a carriage return: on a row wide enough the
 * redraws stay on one row; the bar and " 7%" are 54 columns, so a 53-column terminal wraps it like any other terminal. */
static void bar_redraws(pal_term_t *t, int n) {
    for (int i = 0; i < n; i++) {
        pal_term_write(t, "\r", 1);
        for (int k = 0; k < 2; k++) pal_term_write(t, "\xe2\x96\xa0", 3);
        for (int k = 0; k < 48; k++) pal_term_write(t, "\xef\xbd\xa5", 3);
        pal_term_write(t, "  7%", 4);
    }
}

TEST(progress_bar_with_halfwidth_dots_redraws_on_one_row) {
    pal_term_t *t = pal_term_create(64, 6, 0, NEMA_OWNER_KERNEL);
    bar_redraws(t, 5);
    int cx, cy;
    bool vis;
    pal_term_cursor(t, &cx, &cy, &vis);
    CHECK_EQ(cy, 0);
    CHECK_EQ(cx, 54);
    CHECK_EQ(pal_term_cell(t, 10, 0)->ch, 0xFF65u);
    CHECK(!(pal_term_cell(t, 10, 0)->attr & PAL_TERM_ATTR_WIDE));
    pal_term_destroy(t);
}

TEST(fallback_font_covers_scripts_symbols_and_icons) {
    int cells = 0;
    const uint8_t *g = pal_fallback_glyph(0x0905, &cells); /* Devanagari A */
    CHECK(g != NULL);
    CHECK_EQ(cells, 2); /* Unifont draws the Indic scripts 16 pixels wide; the terminal squeezes them into the one column they take */
    g = pal_fallback_glyph(0x05E9, &cells); /* Hebrew shin */
    CHECK(g != NULL);
    CHECK_EQ(cells, 1);
    g = pal_fallback_glyph(0xF07B, &cells); /* Nerd Font folder */
    CHECK(g != NULL);
    CHECK_EQ(cells, 1);
    bool any = false;
    for (int i = 0; i < 16; i++) any |= g[i] != 0;
    CHECK(any); /* an icon with ink, not an empty cell */
    g = pal_fallback_glyph(0x1F680, &cells); /* rocket: an emoji outline, two cells */
    CHECK(g != NULL);
    CHECK_EQ(cells, 2);
    CHECK(pal_fallback_glyph(0x10FFFD, &cells) == NULL); /* nobody has it: the terminal draws an empty box */
    CHECK(pal_fallback_glyph(0x2500, &cells) == NULL);   /* box drawing is drawn by the terminal, not carried */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(text_and_newlines);
    RUN(autowrap_uses_pending_wrap);
    RUN(cursor_addressing_and_erase);
    RUN(scrolling_feeds_scrollback);
    RUN(history_is_freed_under_pressure);
    RUN(sgr_colours_and_attributes);
    RUN(alternate_screen_restores_main);
    RUN(scroll_region_like_an_editor);
    RUN(insert_and_delete);
    RUN(tabs);
    RUN(utf8_and_line_drawing);
    RUN(device_status_replies);
    RUN(modes_are_tracked);
    RUN(sequences_split_across_writes);
    RUN(strings_are_swallowed);
    RUN(resize_keeps_content);
    RUN(drawing_honours_colours_and_cursor);
    RUN(full_screen_program_session);
    RUN(wide_characters_take_two_columns);
    RUN(combining_marks_take_no_column);
    RUN(wide_character_in_the_last_column_wraps_first);
    RUN(overwriting_half_a_wide_character_blanks_the_other_half);
    RUN(progress_bar_with_halfwidth_dots_redraws_on_one_row);
    RUN(fallback_font_covers_scripts_symbols_and_icons);
    pal_term_destroy(T);
    return TEST_DONE();
}
