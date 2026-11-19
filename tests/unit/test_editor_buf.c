/* The text editor's gap buffer (apps/editor_buf.c): insert and erase at the cursor, moves, line arithmetic, find, and a full buffer. */
#include <string.h>

#include "../../apps/editor_buf.h"
#include "test.h"

static char mem[64];
static ebuf_t B;

static void text(char *out, size_t cap) {
    size_t n = eb_copy(&B, 0, out, cap - 1);
    out[n] = 0;
}

static void put(const char *s) { CHECK(eb_insert(&B, s, strlen(s))); }

TEST(insert_move_and_erase_keep_the_text_in_order) {
    eb_init(&B, mem, sizeof(mem));
    put("hello world");
    eb_move(&B, 5);
    put(",");
    char t[64];
    text(t, sizeof(t));
    CHECK(!strcmp(t, "hello, world"));
    CHECK_EQ((int)eb_cursor(&B), 6);
    CHECK_EQ((int)eb_delete_back(&B, 2), 2);
    text(t, sizeof(t));
    CHECK(!strcmp(t, "hell world"));
    CHECK_EQ((int)eb_delete_fwd(&B, 100), 6); /* only what is there */
    text(t, sizeof(t));
    CHECK(!strcmp(t, "hell"));
    eb_move(&B, 0);
    CHECK_EQ(eb_at(&B, 0), 'h');
    CHECK_EQ(eb_at(&B, 4), 0);
    CHECK_EQ((int)eb_delete_back(&B, 1), 0); /* nothing before the start */
}

TEST(lines_are_counted_and_found) {
    eb_init(&B, mem, sizeof(mem));
    put("one\ntwo\n\nfour");
    CHECK_EQ((int)eb_lines(&B), 4);
    CHECK_EQ((int)eb_line_pos(&B, 1), 4);
    CHECK_EQ((int)eb_line_pos(&B, 2), 8);
    CHECK_EQ((int)eb_line_pos(&B, 3), 9);
    CHECK_EQ((int)eb_line_pos(&B, 9), (int)eb_len(&B)); /* past the last line: the end */
    CHECK_EQ((int)eb_line_of(&B, 5), 1);
    CHECK_EQ((int)eb_line_start(&B, 6), 4);
    CHECK_EQ((int)eb_line_end(&B, 4), 7);
    CHECK_EQ((int)eb_line_end(&B, 9), 13);
    eb_init(&B, mem, sizeof(mem));
    CHECK_EQ((int)eb_lines(&B), 1); /* an empty text is one empty line */
}

TEST(find_is_case_insensitive_and_wraps) {
    eb_init(&B, mem, sizeof(mem));
    put("Alpha beta ALPHA");
    CHECK_EQ((int)eb_find(&B, 0, "alpha"), 0);
    CHECK_EQ((int)eb_find(&B, 1, "alpha"), 11);
    CHECK_EQ((int)eb_find(&B, 12, "alpha"), 0); /* wraps to the start */
    CHECK_EQ((int)eb_find(&B, 0, "gamma"), -1);
    CHECK_EQ((int)eb_find(&B, 0, ""), -1);
}

TEST(a_full_buffer_refuses_more) {
    char small[8];
    ebuf_t b;
    eb_init(&b, small, sizeof(small));
    CHECK(eb_insert(&b, "12345678", 8));
    CHECK(!eb_insert(&b, "9", 1));
    CHECK_EQ((int)eb_len(&b), 8);
    eb_move(&b, 3);
    CHECK_EQ(eb_at(&b, 3), '4'); /* a move in a full buffer keeps every byte */
    CHECK_EQ(eb_at(&b, 7), '8');
}

int main(void) {
    RUN(insert_move_and_erase_keep_the_text_in_order);
    RUN(lines_are_counted_and_found);
    RUN(find_is_case_insensitive_and_wraps);
    RUN(a_full_buffer_refuses_more);
    return TEST_DONE();
}
