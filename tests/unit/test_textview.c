/* The text viewer's model (apps/textview_model.c): UTF-8 decoding, the binary test, soft-wrapped rows (tabs, long words, line
 * numbers, the row table limit), the search (case-insensitive, next match, all matches on screen) and the hex dump rows. */
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "test.h"
#include "textview_model.h"

static textview_t make(const char *text) {
    textview_t t;
    memset(&t, 0, sizeof(t));
    t.data = (uint8_t *)malloc(TV_MAX_BYTES);
    t.rows = (tv_row_t *)malloc(TV_MAX_ROWS * sizeof(tv_row_t));
    t.len = strlen(text);
    memcpy(t.data, text, t.len);
    return t;
}

static void drop(textview_t *t) {
    free(t->data);
    free(t->rows);
}

TEST(decode_utf8_sequences) {
    uint32_t cp = 0;
    const uint8_t a[] = {'A'};
    CHECK_EQ(tv_decode(a, 1, &cp), 1);
    CHECK_EQ(cp, 'A');
    const uint8_t e[] = {0xC3, 0xA9}; /* e acute */
    CHECK_EQ(tv_decode(e, 2, &cp), 2);
    CHECK_EQ(cp, 0xE9);
    const uint8_t euro[] = {0xE2, 0x82, 0xAC};
    CHECK_EQ(tv_decode(euro, 3, &cp), 3);
    CHECK_EQ(cp, 0x20AC);
    const uint8_t bad[] = {0xC3, 0x28};
    CHECK_EQ(tv_decode(bad, 2, &cp), 1);
    CHECK_EQ(cp, 0xFFFD);
    CHECK_EQ(tv_decode(euro, 2, &cp), 1); /* cut short at the end of the buffer */
}

TEST(binary_test_looks_at_nul_and_control_bytes) {
    CHECK(!tv_looks_binary((const uint8_t *)"plain text\n\twith tabs\r\n", 23));
    CHECK(tv_looks_binary((const uint8_t *)"abc\0def", 7));
    uint8_t ctl[20];
    memset(ctl, 0x01, sizeof(ctl));
    CHECK(tv_looks_binary(ctl, sizeof(ctl)));
    CHECK(!tv_looks_binary((const uint8_t *)"", 0));
}

TEST(wraps_long_lines_at_the_column_count) {
    textview_t t = make("abcdefghijklmnopqrst");        /* 20 characters */
    CHECK_EQ(tv_layout(&t, 8), 3);                      /* 8 + 8 + 4 */
    CHECK_EQ(t.rows[0].start, 0);
    CHECK_EQ(t.rows[0].end, 8);
    CHECK_EQ(t.rows[1].start, 8);
    CHECK_EQ(t.rows[2].end, 20);
    CHECK_EQ(t.rows[0].line, 1);
    CHECK_EQ(t.rows[1].line, 0); /* a continuation row is not a line start */
    CHECK_EQ(t.lines, 1);
    drop(&t);
}

TEST(newlines_end_rows_and_count_lines) {
    textview_t t = make("one\ntwo\r\n\nfour");
    CHECK_EQ(tv_layout(&t, 40), 4);
    CHECK_EQ(t.lines, 4);
    CHECK_EQ(t.rows[0].end, 3);       /* "one" without the newline */
    CHECK_EQ(t.rows[1].end, 8);       /* "two\r": the carriage return is in the row, but drawn as nothing */
    CHECK_EQ(t.rows[2].end, t.rows[2].start); /* the empty line */
    CHECK_EQ(t.rows[3].line, 4);
    drop(&t);
}

TEST(newline_right_after_a_full_row_makes_no_empty_row) {
    textview_t t = make("abcd\nef");
    CHECK_EQ(tv_layout(&t, 4), 2);
    CHECK_EQ(t.rows[0].end, 4);
    CHECK_EQ(t.rows[1].start, 5);
    drop(&t);
}

TEST(tabs_go_to_the_next_multiple_of_four) {
    textview_t t = make("a\tb\tcdefgh");
    CHECK_EQ(tv_layout(&t, 10), 2); /* a, tab to col 4, b, tab to col 8, c, d (col 10) | e f g h */
    CHECK_EQ(t.rows[0].end, 6);
    drop(&t);
}

TEST(empty_text_has_no_rows) {
    textview_t t = make("");
    CHECK_EQ(tv_layout(&t, 20), 0);
    CHECK_EQ(tv_row_of(&t, 0), 0);
    drop(&t);
}

TEST(row_table_limit_is_reported) {
    /* more lines than rows: the layout stops at TV_MAX_ROWS and says so */
    textview_t t = make("");
    t.len = TV_MAX_ROWS + 100;
    for (size_t i = 0; i < t.len; i++) t.data[i] = '\n';
    CHECK_EQ(tv_layout(&t, 20), TV_MAX_ROWS);
    CHECK(t.rows_full);
    drop(&t);
}

TEST(search_is_case_insensitive_and_finds_next) {
    textview_t t = make("Lorem ipsum\nlorem again\nno match here\nLOREM end");
    tv_layout(&t, 40);
    CHECK_EQ(tv_find(&t, 0, "lorem", true), 0);
    CHECK_EQ(tv_find(&t, 1, "lorem", true), 12);
    CHECK_EQ(tv_find(&t, 13, "lorem", true), 38);
    CHECK_EQ(tv_find(&t, 39, "lorem", true), -1);
    CHECK_EQ(tv_find(&t, 0, "lorem", false), 12); /* case-sensitive: "Lorem" at 0 does not count */
    CHECK_EQ(tv_find(&t, 0, "", true), -1);
    CHECK_EQ(tv_row_of(&t, 12), 1);
    CHECK_EQ(tv_row_of(&t, 38), 3);
    drop(&t);
}

TEST(matches_on_screen_are_collected) {
    textview_t t = make("ab ab\nxab\nnothing");
    tv_layout(&t, 40);
    uint32_t starts[8];
    int n = tv_matches(&t, 0, 1, "ab", true, starts, 8);
    CHECK_EQ(n, 3);
    CHECK_EQ(starts[0], 0);
    CHECK_EQ(starts[1], 3);
    CHECK_EQ(starts[2], 7);
    CHECK_EQ(tv_matches(&t, 2, 2, "ab", true, starts, 8), 0);
    drop(&t);
}

TEST(hex_rows_cover_the_file) {
    textview_t t = make("12345678901");
    CHECK_EQ(tv_hex_rows(&t), 2); /* 8 + 3 */
    t.len = 0;
    CHECK_EQ(tv_hex_rows(&t), 0);
    drop(&t);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(decode_utf8_sequences);
    RUN(binary_test_looks_at_nul_and_control_bytes);
    RUN(wraps_long_lines_at_the_column_count);
    RUN(newlines_end_rows_and_count_lines);
    RUN(newline_right_after_a_full_row_makes_no_empty_row);
    RUN(tabs_go_to_the_next_multiple_of_four);
    RUN(empty_text_has_no_rows);
    RUN(row_table_limit_is_reported);
    RUN(search_is_case_insensitive_and_finds_next);
    RUN(matches_on_screen_are_collected);
    RUN(hex_rows_cover_the_file);
    return TEST_DONE();
}
