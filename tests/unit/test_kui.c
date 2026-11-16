/* kui (system/src/kui): the logical 1-bit canvas and its primitives, the reference text widths, the expansion of a logical
 * canvas into the 8-bit physical framebuffer (2 px per logical pixel, ink and paper only), and the status bar's pixels. */
#include <stdint.h>
#include <string.h>

#include "pal/gfx.h"
#include "pal/kui.h"
#include "pal/theme.h"
#include "test.h"

static uint8_t PHYS[320 * 240];

TEST(text_width_is_the_sum_of_advances_without_trailing_spacing) {
    CHECK_EQ(kui_text_w(&FONT_REG8, ""), 0);
    int a = kui_text_w(&FONT_REG8, "A"), o = kui_text_w(&FONT_REG8, "O");
    CHECK(a > 0 && o > 0);
    CHECK_EQ(kui_text_w(&FONT_REG8, "AO"), a + o + FONT_REG8.spacing - FONT_REG8.spacing); /* spacing is 0 in the reference */
    /* a monospace font: every glyph has the same advance */
    CHECK_EQ(kui_text_w(&FONT_6X8, "abc"), 3 * 6);
    /* a character outside the font takes the fallback advance (charW) */
    CHECK_EQ(kui_text_w(&FONT_6X8, "\x01"), 6);
}

TEST(canvas_primitives_fill_invert_and_clip) {
    static kui_canvas_t c;
    kui_canvas_init(&c, 320, 240, 8);
    CHECK_EQ(c.w, 160);
    CHECK_EQ(c.h, 120);
    kui_fill(&c, 3, 4, 5, 2, true);
    CHECK(kui_get(&c, 3, 4) && kui_get(&c, 7, 5));
    CHECK(!kui_get(&c, 8, 5) && !kui_get(&c, 3, 6));
    kui_invert(&c, 3, 4, 5, 1);
    CHECK(!kui_get(&c, 3, 4) && kui_get(&c, 3, 5)); /* the row inverted, the one below kept */
    /* clip: nothing is written outside it */
    kui_clip(&c, 10, 10, 4, 4);
    kui_fill(&c, 0, 0, 160, 120, true);
    CHECK(kui_get(&c, 10, 10) && kui_get(&c, 13, 13));
    CHECK(!kui_get(&c, 9, 10) && !kui_get(&c, 14, 10));
    kui_clip_reset(&c);
}

TEST(round_rect_cuts_the_corners_and_frame_is_an_outline) {
    static kui_canvas_t c;
    kui_canvas_init(&c, 320, 240, 8);
    kui_fill_round(&c, 2, 2, 10, 6, 1, true);
    CHECK(!kui_get(&c, 2, 2)); /* corner clipped (radius 1) */
    CHECK(kui_get(&c, 3, 2) && kui_get(&c, 2, 3));
    kui_frame(&c, 20, 2, 6, 4, true);
    CHECK(kui_get(&c, 20, 2) && kui_get(&c, 25, 5));
    CHECK(!kui_get(&c, 22, 3)); /* inside the outline stays empty */
}

TEST(present_expands_each_logical_pixel_to_ink_or_paper) {
    static kui_canvas_t c;
    pal_canvas_t pc;
    pal_canvas_init(&pc, PHYS, 320, 240);
    memset(PHYS, 0, sizeof(PHYS));
    kui_canvas_init(&c, 320, 240, 8);
    kui_pixel(&c, 1, 1, true);
    kui_present(&pc, &c, (pal_rect_t){0, 0, 8, 8});
    CHECK_EQ(PHYS[2 * 320 + 2], PAL_C_INK);
    CHECK_EQ(PHYS[3 * 320 + 3], PAL_C_INK);
    CHECK_EQ(PHYS[0], PAL_C_PAPER);
    CHECK_EQ(PHYS[4 * 320 + 4], PAL_C_PAPER);
    /* a rectangle that starts inside a logical pixel is still exact */
    kui_present(&pc, &c, (pal_rect_t){3, 3, 2, 2});
    CHECK_EQ(PHYS[3 * 320 + 3], PAL_C_INK);
    CHECK_EQ(PHYS[3 * 320 + 4], PAL_C_PAPER);
}

TEST(palanu_palette_is_the_default_and_dark_mode_swaps_it) {
    const kui_palette_t *p = kui_palette_named("palanu");
    CHECK_EQ(p->ink, 0x292442u);
    CHECK_EQ(p->paper, 0xAD9EEFu);
    CHECK_EQ(kui_palette_named("nope")->ink, 0x292442u); /* unknown names give palanu */
    CHECK_EQ(kui_palette_named("orange")->paper, 0xFF6500u);
    const kui_palette_t *d = kui_palette_dark(p);
    CHECK_EQ(d->ink, 0xAD9EEFu);
    CHECK_EQ(d->paper, 0x292442u);
}

TEST(status_bar_fills_ink_and_draws_clock_and_battery_in_paper) {
    static kui_canvas_t c;
    kui_canvas_init(&c, 320, 240, 8);
    kui_status_t st = {10, 0, 100, true};
    kui_status_draw(&c, &st);
    CHECK(kui_get(&c, 0, 0) && kui_get(&c, 100, 5)); /* the bar is ink */
    CHECK(!kui_get(&c, 145, 2));                     /* battery outline is paper */
    CHECK(!kui_get(&c, 150, 5));                     /* a full charge fills the interior with paper too */
    CHECK(kui_get(&c, 150, 9) == true);              /* below the battery, back in ink */
    CHECK(kui_get(&c, 2, 3) && !kui_get(&c, 3, 3)); /* digit '1': its top middle pixel is paper, the unlit corner stays ink */
}

TEST(scale_in_quarters_sizes_the_canvas_and_maps_rows_both_ways) {
    static kui_canvas_t c;
    const int q[] = {4, 5, 6, 7, 8, 10, 12}, w[] = {320, 256, 213, 182, 160, 128, 106}, h[] = {240, 192, 160, 137, 120, 96, 80};
    for (int i = 0; i < 7; i++) {
        kui_canvas_init(&c, 320, 240, q[i]);
        CHECK_EQ(c.w, w[i]);
        CHECK_EQ(c.h, h[i]);
        for (int y = 0; y < 240; y++) {
            int l = kui_lg(&c, y);
            CHECK(kui_ph(q[i], l) <= y);           /* the first physical row of a logical row is at or above it */
            CHECK(kui_lg(&c, kui_ph(q[i], l)) == l); /* and shows that logical row */
        }
    }
    CHECK_EQ(kui_ph(8, 11), 22); /* the status bar at 2x: 22 px, as before */
    CHECK_EQ(kui_ph(6, 11), 17);
    CHECK_EQ(kui_ph(4, 11), 11);
}

TEST(bars_follow_the_ui_scale) {
    int was = kui_scale_q();
    kui_set_scale_q(8);
    CHECK_EQ(kui_top_px(KUI_STATUS_H), 22);
    CHECK_EQ(kui_bottom_px(240, KUI_LEGEND_H), 22);
    kui_set_scale_q(4);
    CHECK_EQ(kui_top_px(KUI_STATUS_H), 11);
    CHECK_EQ(kui_bottom_px(240, KUI_LEGEND_H), 11);
    kui_set_scale_q(10);
    CHECK_EQ(kui_top_px(KUI_STATUS_H), 28);
    CHECK_EQ(kui_bottom_px(240, KUI_LEGEND_H), 240 - kui_ph(10, 96 - 11));
    kui_set_scale_q(12); /* above the largest: clamped to 2.5x */
    CHECK_EQ(kui_scale_q(), KUI_SCALE_MAX_Q);
    kui_set_scale_q(1); /* clamped */
    CHECK_EQ(kui_scale_q(), KUI_SCALE_MIN_Q);
    kui_set_scale_q(was);
}

int main(void) {
    RUN(text_width_is_the_sum_of_advances_without_trailing_spacing);
    RUN(canvas_primitives_fill_invert_and_clip);
    RUN(round_rect_cuts_the_corners_and_frame_is_an_outline);
    RUN(present_expands_each_logical_pixel_to_ink_or_paper);
    RUN(palanu_palette_is_the_default_and_dark_mode_swaps_it);
    RUN(status_bar_fills_ink_and_draws_clock_and_battery_in_paper);
    RUN(scale_in_quarters_sizes_the_canvas_and_maps_rows_both_ways);
    RUN(bars_follow_the_ui_scale);
    return TEST_DONE();
}
