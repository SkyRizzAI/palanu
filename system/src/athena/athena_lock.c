/* Athena's lock screen: clock, date and PIN dots. The PIN, its check and the lock state are the system's. */
#include <stdio.h>
#include <string.h>

#include "athena_int.h"

/* ---- the lock greeter: Athena's lock screen. The PIN, its check and the lock state are the system's (pal/ui.h, pal/desktop.h). ---- */

void ath_build_lock(void) {
    A.lock = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(A.lock);
    lv_obj_set_pos(A.lock, 0, 0);
    lv_obj_set_size(A.lock, A.w, A.h);
    lv_obj_set_style_bg_color(A.lock, C_WALL_BOT, 0);
    lv_obj_set_style_bg_grad_color(A.lock, C_WALL_TOP, 0);
    lv_obj_set_style_bg_grad_dir(A.lock, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(A.lock, LV_OPA_COVER, 0);
    lv_obj_remove_flag(A.lock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *glyph = ath_text(A.lock, &lv_font_montserrat_16, C_WHITE);
    lv_label_set_text(glyph, LV_SYMBOL_EYE_CLOSE);
    lv_obj_align(glyph, LV_ALIGN_TOP_MID, 0, BAR_H + 6);
    A.lock_clock = ath_text(A.lock, &lv_font_montserrat_48, C_WHITE);
    lv_obj_align(A.lock_clock, LV_ALIGN_TOP_MID, 0, BAR_H + 26);
    A.lock_date = ath_text(A.lock, &lv_font_montserrat_16, C_WHITE);
    lv_obj_set_style_text_opa(A.lock_date, LV_OPA_70, 0);
    lv_obj_align(A.lock_date, LV_ALIGN_TOP_MID, 0, BAR_H + 84);
    A.lock_dots = ath_text(A.lock, &lv_font_montserrat_20, C_WHITE);
    lv_obj_align(A.lock_dots, LV_ALIGN_TOP_MID, 0, BAR_H + 116);
    A.lock_msg = ath_text(A.lock, &lv_font_montserrat_12, lv_color_hex(0xFF6B6B));
    lv_obj_align(A.lock_msg, LV_ALIGN_TOP_MID, 0, BAR_H + 146);
    A.lock_hint = ath_text(A.lock, &lv_font_montserrat_12, C_WHITE);
    lv_obj_set_style_text_opa(A.lock_hint, LV_OPA_70, 0);
    lv_obj_align(A.lock_hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(A.lock, LV_OBJ_FLAG_HIDDEN);
}

void ath_lock_sync(void) {
    bool locked = pal_desktop_locked();
    if (locked != A.lock_shown) {
        A.lock_shown = locked;
        if (locked) lv_obj_remove_flag(A.lock, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(A.lock, LV_OBJ_FLAG_HIDDEN);
        A.lock_dirty = true;
        A.force = true;
    }
    if (!locked || !A.lock_dirty) return;
    A.lock_dirty = false;
    pal_tm_t t;
    pal_time_local(&t);
    lv_label_set_text_fmt(A.lock_clock, "%02d:%02d", t.hour, t.minute);
    char date[40];
    ath_date_text(&t, date, sizeof(date));
    lv_label_set_text(A.lock_date, date);
    bool pin = pal_ui_pin_is_set();
    char dots[3 * PAL_UI_PIN_MAX + 1] = "";
    for (int i = 0; pin && i < A.pin.len && i < PAL_UI_PIN_MAX; i++) strcat(dots, "\xE2\x97\x8F"); /* a filled circle */
    lv_label_set_text(A.lock_dots, dots);
    lv_label_set_text(A.lock_msg, pin && A.pin.last_wrong ? "Wrong PIN" : "");
    lv_label_set_text(A.lock_hint, pin ? "Type the PIN, then Enter" : "Press Enter twice to unlock");
}

static void lock_enter(pal_view_t *v) {
    (void)v;
    pal_ui_pin_reset(&A.pin);
    A.enters = 0;
    A.lock_dirty = true;
}

static bool lock_key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    A.lock_dirty = true;
    if (pal_ui_pin_is_set()) {
        if (pal_ui_pin_key(&A.pin, k, pal_ui_pin_check) == PAL_UI_PIN_OK) pal_ui_unlock();
        return true;
    }
    if (k->sym == PAL_KEY_ENTER) {
        if (++A.enters >= 2) pal_ui_unlock();
    } else {
        A.enters = 0;
    }
    return true;
}

pal_view_t ath_lock_view = {.id = "athena-lock", .title = "Locked", .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_HIDDEN,
                               .enter = lock_enter, .key = lock_key};

