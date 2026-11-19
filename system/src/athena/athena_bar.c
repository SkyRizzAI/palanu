/* Athena's status bar: on LVGL's top layer above every screen, clear over the home and solid over an app; the clock, the radios and
 * the battery (as rising bars on a theme with level_bars), and the app's name over an app. */
#include "athena_int.h"

/* ---- status bar ---- */

static const char *battery_glyph(int pct) {
    if (pct >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60) return LV_SYMBOL_BATTERY_3;
    if (pct >= 35) return LV_SYMBOL_BATTERY_2;
    if (pct >= 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* The status costs an ADC sample for the battery: read it once a second at most, as the classic bar does (30 reads a second
 * tripped the interrupt watchdog on hhv2). */
void ath_bar_update(bool always, uint32_t now) {
    static uint32_t last_read;
    if (!always && now - last_read < 1000u) return;
    last_read = now;
    pal_status_t st = pal_desktop_status();
    if (!always && st.hour == A.st.hour && st.minute == A.st.minute && st.battery == A.st.battery && st.wifi == A.st.wifi &&
        st.ble == A.st.ble && st.link == A.st.link)
        return;
    A.st = st;
    lv_label_set_text_fmt(A.bar_time, "%02d:%02d", st.hour, st.minute);
    char right[48];
    int n = 0;
    right[0] = 0;
    if (st.link) n += snprintf(right + n, sizeof(right) - (size_t)n, "%s  ", LV_SYMBOL_LOOP);
    if (st.ble) n += snprintf(right + n, sizeof(right) - (size_t)n, "%s  ", LV_SYMBOL_BLUETOOTH);
    if (ath_T->level_bars) { /* the signal and the battery are bars: lit up to their level */
        for (int i = 0; i < 4; i++) lv_obj_set_style_bg_opa(A.lvl_sig[i], i < st.wifi ? LV_OPA_COVER : LV_OPA_30, 0);
        int lit = st.battery < 0 ? 0 : (st.battery + 19) / 20;
        for (int i = 0; i < 5; i++) lv_obj_set_style_bg_opa(A.lvl_bat[i], i < lit ? LV_OPA_COVER : LV_OPA_30, 0);
    } else {
        if (st.wifi > 0) n += snprintf(right + n, sizeof(right) - (size_t)n, "%s  ", LV_SYMBOL_WIFI);
        if (st.battery >= 0) snprintf(right + n, sizeof(right) - (size_t)n, "%d%%  %s", st.battery, battery_glyph(st.battery));
    }
    lv_label_set_text(A.bar_right, right);
}

/* Rising bars at x (left to right), the tallest on the side away from the screen's edge. */
static void level_bars(lv_obj_t **out, int n, int x, bool rise_right) {
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_create(A.bar);
        lv_obj_remove_style_all(b);
        int hh = 4 + (rise_right ? i : n - 1 - i) * 10 / n;
        lv_obj_set_size(b, 3, hh);
        lv_obj_set_pos(b, x + i * 5, BAR_H - 4 - hh);
        lv_obj_set_style_bg_color(b, C_WHITE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
        out[i] = b;
    }
}

/* Over the home the bar is clear (the wallpaper shows through); over an app it is solid black, as the app's own top edge. */
void ath_bar_style(bool solid) {
    if (solid == A.bar_solid) return;
    A.bar_solid = solid;
    lv_obj_set_style_bg_opa(A.bar, solid ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void ath_build_bar(void) {
    A.bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(A.bar);
    lv_obj_set_size(A.bar, A.w, BAR_H);
    lv_obj_set_pos(A.bar, 0, 0);
    lv_obj_set_style_bg_color(A.bar, lv_color_hex(ath_T->bar_bg), 0);
    lv_obj_set_style_bg_opa(A.bar, LV_OPA_TRANSP, 0);
    A.bar_solid = false;
    A.bar_time = lv_label_create(A.bar);
    lv_obj_set_style_text_font(A.bar_time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_time, C_WHITE, 0);
    if (ath_T->level_bars) { /* the signal on the left, the battery on the right, the clock between them */
        level_bars(A.lvl_sig, 4, 8, true);
        level_bars(A.lvl_bat, 5, A.w - 8 - 5 * 5 + 2, false);
        lv_obj_align(A.bar_time, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_align(A.bar_time, LV_ALIGN_LEFT_MID, 12, 0);
    }
    A.bar_title = ath_text(A.bar, &lv_font_montserrat_12, C_WHITE);
    lv_obj_align(A.bar_title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(A.bar_title, "");
    A.bar_toast = lv_label_create(A.bar); /* a toast takes the bar while it shows (athena_shell.c) */
    lv_obj_set_style_text_font(A.bar_toast, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_toast, C_WHITE, 0);
    lv_label_set_long_mode(A.bar_toast, LV_LABEL_LONG_DOT);
    lv_obj_set_width(A.bar_toast, A.w - 24);
    lv_obj_set_style_text_align(A.bar_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(A.bar_toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(A.bar_toast, LV_OBJ_FLAG_HIDDEN);
    A.bar_right = lv_label_create(A.bar);
    lv_obj_set_style_text_font(A.bar_right, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(A.bar_right, C_WHITE, 0);
    lv_obj_align(A.bar_right, LV_ALIGN_RIGHT_MID, ath_T->level_bars ? -40 : -10, 0);
    ath_bar_update(true, 0);
}

