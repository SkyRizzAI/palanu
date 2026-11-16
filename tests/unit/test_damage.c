/* Dirty-rectangle tracking on the canvas (system/src/gfx.c): what a primitive marks, how marks combine, what
 * pal_take_dirty returns, and what a full clear marks. The display only sends what is marked, so these rules
 * decide how much is pushed over SPI. */
#include <string.h>

#include "nema/nema.h"
#include "pal/gfx.h"
#include "test.h"

#define W 64
#define H 48
static uint8_t PX[W * H];
static pal_canvas_t C;

static bool rect_is(pal_rect_t r, int x, int y, int w, int h) {
    bool ok = r.x == x && r.y == y && r.w == w && r.h == h;
    if (!ok) printf("    got {%d,%d,%d,%d} want {%d,%d,%d,%d}\n", r.x, r.y, r.w, r.h, x, y, w, h);
    return ok;
}

static void fresh(void) {
    memset(PX, 0, sizeof(PX));
    pal_canvas_init(&C, PX, W, H);
    pal_rect_t r;
    pal_take_dirty(&C, &r); /* a fresh canvas starts clean */
}

TEST(fresh_canvas_is_clean) {
    fresh();
    pal_rect_t r = {1, 2, 3, 4};
    CHECK(!pal_take_dirty(&C, &r));
    CHECK_EQ(r.w, 3); /* untouched when nothing is dirty */
}

TEST(small_fill_marks_exactly_that_rect) {
    fresh();
    pal_fill(&C, 10, 5, 4, 3, 7);
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    CHECK(rect_is(r, 10, 5, 4, 3));
    CHECK(!pal_take_dirty(&C, &r)); /* taking clears the mark */
}

TEST(single_pixel_marks_one_pixel) {
    fresh();
    pal_pixel(&C, 63, 47, 1);
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    CHECK(rect_is(r, 63, 47, 1, 1));
}

TEST(two_disjoint_fills_give_their_bounding_box) {
    fresh();
    pal_fill(&C, 0, 0, 2, 2, 1);
    pal_fill(&C, 50, 40, 3, 3, 2);
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    /* one rectangle covering both: the smallest one that contains the two, not the sum of their areas */
    CHECK(rect_is(r, 0, 0, 53, 43));
    CHECK(r.w * r.h <= W * H);
    CHECK(r.w * r.h >= 2 * 2 + 3 * 3);
}

TEST(overlapping_fills_merge) {
    fresh();
    pal_fill(&C, 5, 5, 10, 10, 1);
    pal_fill(&C, 10, 8, 10, 10, 1);
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    CHECK(rect_is(r, 5, 5, 15, 13));
}

TEST(clipped_marks_stay_inside_the_clip) {
    fresh();
    pal_clip_set(&C, 0, 0, 8, 8);
    pal_fill(&C, 6, 6, 10, 10, 1); /* only the 2x2 corner inside the clip is painted and marked */
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    CHECK(rect_is(r, 6, 6, 2, 2));
    pal_clip_reset(&C);
    pal_fill(&C, 20, 20, 0, 5, 1); /* empty fills mark nothing */
    CHECK(!pal_take_dirty(&C, &r));
}

TEST(full_clear_marks_everything) {
    fresh();
    pal_clear(&C, 3);
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    CHECK(rect_is(r, 0, 0, W, H));
}

TEST(mark_all_marks_everything) {
    fresh();
    pal_mark_all(&C);
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    CHECK(rect_is(r, 0, 0, W, H));
}

TEST(explicit_mark_and_pixel_reads) {
    fresh();
    pal_mark(&C, -5, -5, 10, 10); /* clipped to the canvas */
    pal_rect_t r;
    CHECK(pal_take_dirty(&C, &r));
    CHECK(rect_is(r, 0, 0, 5, 5));
    pal_fill(&C, 1, 1, 1, 1, 9);
    CHECK_EQ(pal_get(&C, 1, 1), 9);
    fresh();
    CHECK(!pal_take_dirty(&C, &r));
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(fresh_canvas_is_clean);
    RUN(small_fill_marks_exactly_that_rect);
    RUN(single_pixel_marks_one_pixel);
    RUN(two_disjoint_fills_give_their_bounding_box);
    RUN(overlapping_fills_merge);
    RUN(clipped_marks_stay_inside_the_clip);
    RUN(full_clear_marks_everything);
    RUN(mark_all_marks_everything);
    RUN(explicit_mark_and_pixel_reads);
    return TEST_DONE();
}
