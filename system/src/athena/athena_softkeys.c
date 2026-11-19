/* Athena's softkey bar, for a theme that has one (ath_theme_t.softkeys): a band at the bottom of every screen with three labels, the
 * left softkey (Tab: Options, or Menu on the idle home), the Enter action in the middle and the right softkey (Esc: Back, Exit or
 * Cancel). The labels are the session's, set by what is in front (athena.c); the keys are the ones the board already has. */
#include "athena_int.h"

void ath_pane_bg(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(ath_T->pane_top), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(ath_T->pane_bot), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static lv_obj_t *sk_label(lv_align_t align, int x, lv_text_align_t ta) {
    lv_obj_t *l = ath_text(A.sk, &lv_font_montserrat_12, C_WHITE);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, A.w / 3 - 8);
    lv_obj_set_style_text_align(l, ta, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, x, 0);
    return l;
}

void ath_softkeys_build(void) {
    A.sk = A.sk_l = A.sk_c = A.sk_r = NULL;
    if (!ath_T->softkeys) return;
    A.sk = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(A.sk);
    lv_obj_set_size(A.sk, A.w, SK_H);
    lv_obj_set_pos(A.sk, 0, A.h - SK_H);
    ath_pane_bg(A.sk);
    lv_obj_remove_flag(A.sk, LV_OBJ_FLAG_SCROLLABLE);
    A.sk_l = sk_label(LV_ALIGN_LEFT_MID, 6, LV_TEXT_ALIGN_LEFT);
    A.sk_c = sk_label(LV_ALIGN_CENTER, 0, LV_TEXT_ALIGN_CENTER);
    A.sk_r = sk_label(LV_ALIGN_RIGHT_MID, -6, LV_TEXT_ALIGN_RIGHT);
}

static void set_if(lv_obj_t *l, const char *t) {
    if (!l) return;
    const char *now = lv_label_get_text(l);
    if (strcmp(now ? now : "", t ? t : "")) lv_label_set_text(l, t ? t : "");
}

void ath_softkeys_set(const char *left, const char *mid, const char *right) {
    set_if(A.sk_l, left);
    set_if(A.sk_c, mid);
    set_if(A.sk_r, right);
}

void ath_softkeys_show(bool on) {
    if (!A.sk) return;
    if (on == !lv_obj_has_flag(A.sk, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(A.sk, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(A.sk, LV_OBJ_FLAG_HIDDEN);
}
