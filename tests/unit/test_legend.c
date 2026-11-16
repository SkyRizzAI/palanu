/* The legend bar (system/src/legend.c, kui_legend.c, desktop.c): the pure layout (one, two, three and more items; dropping from
 * the right), the board's key caps (boards/hhv2/hhv2_legend.c through HHV2_LEGEND_KEYS, and a hidden action), the drawing into
 * the bottom rows of a 320x240 canvas, and the content area of a view that has legends or NOLEGEND. Set and clear work only on
 * the desktop task, so here they are checked to do nothing before the task runs. */
#include <string.h>

#include "nema/nema.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/legend.h"
#include "test.h"

extern const char *const HHV2_LEGEND_KEYS[];
extern const pal_keyboard_t HHV2_KEYBOARD;

static uint8_t PX[320 * 240];

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    (void)ctx, (void)px, (void)stride, (void)r, (void)pal;
}

/* ---------------------------------------------------------------- layout (pure) */

TEST(layout_one_item_sits_left) {
    int w[1] = {40}, xs[1] = {-1};
    CHECK_EQ(pal_legend_layout(w, 1, 160, xs), 1);
    CHECK_EQ(xs[0], 2);
}

TEST(layout_two_items_left_and_right) {
    int w[2] = {40, 30}, xs[2];
    CHECK_EQ(pal_legend_layout(w, 2, 160, xs), 2);
    CHECK_EQ(xs[0], 2);
    CHECK_EQ(xs[1], 160 - 2 - 30);
}

TEST(layout_three_items_left_centre_right) {
    int w[3] = {40, 40, 40}, xs[3];
    CHECK_EQ(pal_legend_layout(w, 3, 160, xs), 3);
    CHECK_EQ(xs[0], 2);
    CHECK_EQ(xs[1], (160 - 40) / 2);
    CHECK_EQ(xs[2], 160 - 2 - 40);
}

TEST(layout_drops_from_the_right_and_never_overlaps) {
    /* three wide items do not fit as left, centre, right: the last goes, and the two left remain */
    int w[3] = {70, 70, 70}, xs[3];
    int k = pal_legend_layout(w, 3, 160, xs);
    CHECK_EQ(k, 2);
    CHECK(xs[0] + w[0] <= xs[1]);
    CHECK(xs[1] + w[1] <= 160);
}

TEST(layout_packs_more_than_three_left_with_gaps) {
    int w[5] = {20, 20, 20, 20, 20}, xs[5];
    CHECK_EQ(pal_legend_layout(w, 5, 160, xs), 5);
    CHECK_EQ(xs[0], 2);
    CHECK_EQ(xs[1], 2 + 20 + 6);
    CHECK_EQ(xs[4], 2 + 4 * 26);
    for (int i = 1; i < 5; i++) CHECK(xs[i - 1] + w[i - 1] <= xs[i]);
}

TEST(layout_packed_drops_the_tail_that_does_not_fit) {
    int w[6] = {30, 30, 30, 30, 30, 30}, xs[6];
    int k = pal_legend_layout(w, 6, 160, xs);
    CHECK(k >= 4 && k < 6);
    CHECK(xs[k - 1] + w[k - 1] <= 160 - 2);
}

TEST(layout_nothing_fits_or_nothing_asked) {
    int w[2] = {200, 10}, xs[2];
    CHECK_EQ(pal_legend_layout(w, 1, 160, xs), 0);
    CHECK_EQ(pal_legend_layout(w, 0, 160, xs), 0);
    CHECK_EQ(pal_legend_layout(NULL, 2, 160, xs), 0);
}

TEST(layout_caps_at_the_maximum) {
    int w[PAL_LEGEND_MAX + 3], xs[PAL_LEGEND_MAX + 3];
    for (int i = 0; i < PAL_LEGEND_MAX + 3; i++) w[i] = 1;
    CHECK_EQ(pal_legend_layout(w, PAL_LEGEND_MAX + 3, 1000, xs), PAL_LEGEND_MAX);
}

/* ---------------------------------------------------------------- the board's key caps */

TEST(board_keys_for_hhv2_come_from_its_table) {
    pal_board_desc_t d = {.name = "test", .legend_keys = HHV2_LEGEND_KEYS};
    pal_board_set(&d);
    CHECK(strcmp(pal_legend_board_key(PAL_ACT_OK), KUI_LEGEND_ENTER) == 0); /* the Enter symbol, not the word */
    CHECK(strcmp(pal_legend_board_key(PAL_ACT_BACK), "ESC") == 0);
    CHECK(strcmp(pal_legend_board_key(PAL_ACT_TAB), "TAB") == 0);
    CHECK(strcmp(pal_legend_board_key(PAL_ACT_MOVE_V), "Fn+W/S") == 0);
    CHECK(strcmp(pal_legend_board_key(PAL_ACT_HOME), "Fn+Q") == 0);
}

TEST(board_action_without_a_key_is_hidden) {
    pal_board_desc_t d = {.name = "test", .legend_keys = HHV2_LEGEND_KEYS};
    pal_board_set(&d);
    CHECK(pal_legend_board_key(PAL_ACT_MENU) == NULL); /* hhv2 has no menu key */
    CHECK(pal_legend_board_key(PAL_ACT_NONE) == NULL);
    CHECK(pal_legend_board_key(PAL_ACT_CUSTOM) == NULL); /* custom needs key_text from the app */
    CHECK(pal_legend_board_key(PAL_ACT_COUNT) == NULL);
}

TEST(board_without_a_table_hides_everything) {
    pal_board_desc_t d = {.name = "none"};
    pal_board_set(&d);
    CHECK(pal_legend_board_key(PAL_ACT_OK) == NULL);
}

/* ---------------------------------------------------------------- drawing */

static bool ink_in_rows(int y0, int y1) {
    pal_canvas_t c;
    pal_canvas_init(&c, PX, 320, 240);
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < 320; x++)
            if (pal_get(&c, x, y) == PAL_C_INK) return true;
    return false;
}

TEST(draw_puts_legends_only_in_the_bottom_bar) {
    pal_board_desc_t d = {.name = "test", .legend_keys = HHV2_LEGEND_KEYS};
    pal_board_set(&d);
    pal_canvas_t c;
    pal_canvas_init(&c, PX, 320, 240);
    pal_clear(&c, PAL_C_PAPER);
    pal_legend_item_t items[] = {{PAL_ACT_OK, "open", NULL}, {PAL_ACT_MENU, "menu", NULL}, {PAL_ACT_BACK, "back", NULL}};
    pal_legend_draw(&c, items, 3);
    CHECK(ink_in_rows(240 - pal_legend_h_px(), 240)); /* the cap and labels are drawn */
    CHECK(!ink_in_rows(0, 240 - pal_legend_h_px()));  /* nothing above the bar */
}

TEST(draw_hidden_only_still_paints_the_bar_paper) {
    pal_board_desc_t d = {.name = "none"};
    pal_board_set(&d);
    pal_canvas_t c;
    pal_canvas_init(&c, PX, 320, 240);
    pal_clear(&c, PAL_C_INK);
    pal_legend_item_t items[] = {{PAL_ACT_OK, "open", NULL}};
    pal_legend_draw(&c, items, 1);
    CHECK(!ink_in_rows(240 - pal_legend_h_px(), 240)); /* the hidden legend leaves the strip blank (paper) */
}

TEST(custom_key_text_is_drawn_without_a_board_key) {
    pal_board_desc_t d = {.name = "none"};
    pal_board_set(&d);
    pal_canvas_t c;
    pal_canvas_init(&c, PX, 320, 240);
    pal_clear(&c, PAL_C_PAPER);
    pal_legend_item_t items[] = {{PAL_ACT_CUSTOM, "run", "X"}};
    pal_legend_draw(&c, items, 1);
    CHECK(ink_in_rows(240 - pal_legend_h_px(), 240));
}

/* ---------------------------------------------------------------- the content area and set/clear */

static pal_view_t V_PLAIN = {.id = "legend-plain", .title = "plain"};
static pal_view_t V_LEG = {.id = "legend-with", .title = "with"};
static pal_view_t V_NO = {.id = "legend-no", .title = "no", .flags = PAL_VIEW_NOLEGEND};
static const pal_legend_item_t LEG[] = {{PAL_ACT_OK, "open", NULL}};

TEST(area_shrinks_above_the_bar_only_for_a_view_with_legends) {
    /* the desktop is not started, so the front is the first opened view */
    pal_legend_home_only(false); /* the bar wherever a view declares legends: the rule of this test */
    pal_desktop_open("legend-plain");
    pal_rect_t plain = pal_desktop_area(&V_PLAIN);
    CHECK_EQ(plain.y + plain.h, 240);
    V_PLAIN.legends = NULL;
    V_PLAIN.nlegends = 0;
    V_LEG.legends = LEG;
    V_LEG.nlegends = 1;
    pal_desktop_open("legend-with");
    pal_rect_t with = pal_desktop_area(&V_LEG);
    CHECK_EQ(with.y + with.h, 240 - pal_legend_h_px());
    CHECK_EQ(with.h, plain.h - pal_legend_h_px());
}

TEST(legends_show_on_the_home_screen_only_by_default) {
    pal_legend_home_only(true);
    pal_desktop_open("legend-with");
    CHECK(strcmp(pal_desktop_home_id(), "legend-with") != 0);
    pal_rect_t away = pal_desktop_area(&V_LEG); /* declares legends, is not home: no bar, the whole screen */
    CHECK_EQ(away.y + away.h, 240);
    pal_desktop_set_home("legend-with");
    pal_rect_t home = pal_desktop_area(&V_LEG); /* the same view as the home screen: the bar */
    CHECK_EQ(home.y + home.h, 240 - pal_legend_h_px());
    pal_desktop_set_home("launcher");
    pal_legend_home_only(false);
}

TEST(nolegend_flag_gives_no_bar) {
    V_NO.legends = LEG;
    V_NO.nlegends = 1;
    pal_desktop_open("legend-no");
    pal_rect_t r = pal_desktop_area(&V_NO);
    CHECK_EQ(r.y + r.h, 240);
}

TEST(set_and_clear_do_nothing_off_the_desktop_task) {
    /* only the desktop task changes legends; from here the view keeps what it had */
    pal_legend_set(LEG, 1);
    CHECK(V_LEG.nlegends == 1 && V_LEG.legends == LEG);
    pal_legend_clear();
    CHECK(V_LEG.nlegends == 1);
}

/* ---------------------------------------------------------------- the text form an app publishes */

TEST(parse_actions_labels_and_custom_caps) {
    pal_legend_item_t it[PAL_LEGEND_MAX];
    char buf[160];
    int n = pal_legend_parse(" ok = Jump ;BACK=Quit;[SPACE]=Pause;move_v;;", it, PAL_LEGEND_MAX, buf, sizeof(buf));
    CHECK_EQ(n, 4);
    CHECK_EQ(it[0].action, PAL_ACT_OK);
    CHECK(strcmp(it[0].label, "Jump") == 0);
    CHECK(it[0].key_text == NULL);
    CHECK_EQ(it[1].action, PAL_ACT_BACK); /* names are case-blind */
    CHECK(strcmp(it[1].label, "Quit") == 0);
    CHECK_EQ(it[2].action, PAL_ACT_CUSTOM);
    CHECK(strcmp(it[2].key_text, "SPACE") == 0);
    CHECK(strcmp(it[2].label, "Pause") == 0);
    CHECK_EQ(it[3].action, PAL_ACT_MOVE_V);
    CHECK(it[3].label == NULL); /* the cap alone */
}

TEST(parse_empty_text_is_no_items) {
    pal_legend_item_t it[2];
    char buf[8];
    CHECK_EQ(pal_legend_parse("", it, 2, buf, sizeof(buf)), 0);
    CHECK_EQ(pal_legend_parse(NULL, it, 2, buf, sizeof(buf)), 0);
    CHECK_EQ(pal_legend_parse(" ; ;", it, 2, buf, sizeof(buf)), 0);
}

TEST(parse_refuses_malformed_text) {
    pal_legend_item_t it[2];
    char buf[64];
    CHECK_EQ(pal_legend_parse("jump=Go", it, 2, buf, sizeof(buf)), -1);              /* unknown name */
    CHECK_EQ(pal_legend_parse("[]=Go", it, 2, buf, sizeof(buf)), -1);                /* empty cap */
    CHECK_EQ(pal_legend_parse("[SPACE=Go", it, 2, buf, sizeof(buf)), -1);            /* unclosed */
    CHECK_EQ(pal_legend_parse("[ABCDEFGHIJKLMNOP]", it, 2, buf, sizeof(buf)), -1);   /* cap over 15 */
    CHECK_EQ(pal_legend_parse("=Go", it, 2, buf, sizeof(buf)), -1);                  /* label without a key */
    CHECK_EQ(pal_legend_parse("ok;back;tab", it, 2, buf, sizeof(buf)), -1);          /* more than max */
    CHECK_EQ(pal_legend_parse("ok=Open", it, 2, buf, 4), -1);                        /* buf too small */
    CHECK_EQ(pal_legend_parse("[ABCDEFGHIJKLMNO]=x", it, 2, buf, sizeof(buf)), 1);   /* 15 is allowed */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_input_init(&HHV2_KEYBOARD);
    pal_display_ops_t ops = {.width = 320, .height = 240, .present = present, .backlight = NULL, .ctx = NULL};
    pal_display_init(&ops);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    CHECK(pal_desktop_register(&V_PLAIN) == NEMA_OK);
    CHECK(pal_desktop_register(&V_LEG) == NEMA_OK);
    CHECK(pal_desktop_register(&V_NO) == NEMA_OK);
    RUN(layout_one_item_sits_left);
    RUN(layout_two_items_left_and_right);
    RUN(layout_three_items_left_centre_right);
    RUN(layout_drops_from_the_right_and_never_overlaps);
    RUN(layout_packs_more_than_three_left_with_gaps);
    RUN(layout_packed_drops_the_tail_that_does_not_fit);
    RUN(layout_nothing_fits_or_nothing_asked);
    RUN(layout_caps_at_the_maximum);
    RUN(board_keys_for_hhv2_come_from_its_table);
    RUN(board_action_without_a_key_is_hidden);
    RUN(board_without_a_table_hides_everything);
    RUN(draw_puts_legends_only_in_the_bottom_bar);
    RUN(draw_hidden_only_still_paints_the_bar_paper);
    RUN(custom_key_text_is_drawn_without_a_board_key);
    RUN(area_shrinks_above_the_bar_only_for_a_view_with_legends);
    RUN(legends_show_on_the_home_screen_only_by_default);
    RUN(nolegend_flag_gives_no_bar);
    RUN(set_and_clear_do_nothing_off_the_desktop_task);
    RUN(parse_actions_labels_and_custom_caps);
    RUN(parse_empty_text_is_no_items);
    RUN(parse_refuses_malformed_text);
    return TEST_DONE();
}
