/* App screens (system/src/appui.c behind api/palanu/ui.h): a form's fields become a bound menu any session can draw, its rows' changes
 * come back as events; a canvas's pixels reach the screen frame after the compositor's frame; one screen per app, two apps at most;
 * the screen goes when the app ends. No desktop task runs: the calls that would post run at once. */
#include <string.h>

#include "nema/nema.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/menu.h"
#include "pal/theme.h"
#include "test.h"
#include "../../system/src/appui.h"

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) { (void)ctx, (void)px, (void)stride, (void)r, (void)pal; }
static void present565(void *ctx, const uint16_t *px, int stride, pal_rect_t r) { (void)ctx, (void)px, (void)stride, (void)r; }

static int APP1, APP2, APP3; /* stand-ins for three apps' contexts */

TEST(a_form_is_a_bound_menu_and_its_changes_are_events) {
    CHECK(appui_form_open(&APP1, "Lamp") == 0);
    CHECK(strcmp(pal_desktop_front_id(), "app-screen-0") == 0);
    CHECK(appui_form_add(&APP1, 1, 3, "On", "1") == 0);           /* toggle */
    CHECK(appui_form_add(&APP1, 2, 4, "Mode", "Warm|Cool") == 0); /* choice */
    CHECK(appui_form_add(&APP1, 3, 5, "Level", "0|100|10|60") == 0);
    CHECK(appui_form_add(&APP1, 4, 6, "Go", "") == 0); /* action */
    CHECK(appui_form_add(&APP1, 4, 6, "Again", "") == NEMA_ERR_EXIST);
    CHECK(appui_form_add(&APP1, 9, 99, "Bad", "") == NEMA_ERR_INVAL);
    pal_view_t *v = pal_desktop_view("app-screen-0");
    v->pump(v); /* the desktop builds the page */
    pal_menu_t *m = pal_menu_bound("app-screen-0");
    CHECK(m != NULL);
    pal_menu_page_t *p = pal_menu_top(m);
    CHECK(p && p->n == 4);
    CHECK(pal_menu_select(m, 0));
    CHECK(pal_menu_activate(m)); /* the toggle flips */
    int32_t ev[3];
    CHECK(appui_form_next(&APP1, ev, 0) == 1);
    CHECK(ev[0] == 1 && ev[1] == 1 && ev[2] == 0);
    CHECK(pal_menu_select(m, 3));
    CHECK(pal_menu_activate(m)); /* the action */
    CHECK(appui_form_next(&APP1, ev, 0) == 1 && ev[0] == 4 && ev[1] == 2);
    CHECK(appui_form_next(&APP1, ev, 0) == 0); /* nothing more */
    char t[16];
    CHECK(appui_form_get(&APP1, 2, t, sizeof(t)) == 4 && !strcmp(t, "Warm"));
    CHECK(appui_form_set(&APP1, 3, "250") == 0);
    CHECK(appui_form_get(&APP1, 3, t, sizeof(t)) > 0 && !strcmp(t, "100")); /* clamped to the slider's range */
}

TEST(one_screen_per_app_two_apps_at_most) {
    CHECK(appui_canvas_open(&APP1, "x") == NEMA_ERR_BUSY); /* APP1 has a form */
    int32_t size = appui_canvas_open(&APP2, "Paint");
    CHECK(size > 0);
    CHECK(appui_form_open(&APP3, "Third") == NEMA_ERR_BUSY); /* both slots are taken */
}

TEST(a_canvas_reaches_the_screen_frame_after_the_frame) {
    int32_t size = appui_canvas_open(&APP2, "Paint");
    int w = size >> 16, h = size & 0xFFFF;
    CHECK(w == 320 && h > 0);
    uint8_t row[2 * 4] = {0x1F, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x1F, 0x00}; /* blue */
    CHECK(appui_canvas_present(&APP2, row, sizeof(row), 2, 3, 4) == 0);
    CHECK(appui_canvas_present(&APP2, row, 6, 2, 3, 4) == NEMA_ERR_INVAL); /* not whole rows */
    CHECK(appui_canvas_present(&APP2, row, sizeof(row), w - 2, 0, 4) == NEMA_ERR_INVAL); /* outside */
    appui_after_render();
    pal_rect_t a = pal_desktop_area(pal_desktop_view("app-screen-1"));
    const uint16_t *f = pal_display_frame();
    CHECK(f[(a.y + 3) * 320 + a.x + 2] == 0x001F);
    pal_key_t k = {PAL_KEY_LEFT, 0, false, 0};
    pal_view_t *v = pal_desktop_view("app-screen-1");
    CHECK(v->key(v, &k));
    uint8_t rec[8];
    CHECK(appui_canvas_key(&APP2, rec, 0) == 1 && rec[0] == PAL_KEY_LEFT);
}

TEST(the_screen_goes_with_the_app) {
    appui_release(&APP1);
    CHECK(appui_form_add(&APP1, 5, 2, "x", "y") == NEMA_ERR_NOENT);
    CHECK(appui_form_open(&APP3, "Third") == 0); /* the slot is free again */
    appui_release(&APP2);
    appui_release(&APP3);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_display_ops_t ops = {.width = 320, .height = 240, .present = present, .present_rgb565 = present565};
    pal_display_init(&ops);
    pal_theme_select(0, false, 1);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc);
    appui_init();
    RUN(a_form_is_a_bound_menu_and_its_changes_are_events);
    RUN(one_screen_per_app_two_apps_at_most);
    RUN(a_canvas_reaches_the_screen_frame_after_the_frame);
    RUN(the_screen_goes_with_the_app);
    return TEST_DONE();
}
