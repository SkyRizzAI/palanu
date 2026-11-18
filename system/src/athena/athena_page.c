/* Athena: a bound menu (pal_menu_bind) drawn as a grouped list in full colour. */
#include <stdio.h>
#include <string.h>

#include "athena_int.h"

/* ---- toolkit pages: a bound menu (Settings) drawn in Athena's look ---- */

pal_menu_t *ath_page_menu(void) {
    if (pal_desktop_overlay_count() || pal_desktop_locked()) return NULL;
    return pal_menu_bound(pal_desktop_front_id());
}

static bool selectable(const pal_menu_row_t *r) {
    return r->kind != PAL_MENU_SECTION && !r->inert && !(r->kind == PAL_MENU_SLIDER && !r->set);
}

static void page_value(int i) {
    const pal_menu_row_t *r = &A.ppage->rows[A.pidx[i]];
    if (!A.pval[i]) return;
    char v[VALUE_MAX];
    if (r->kind == PAL_MENU_TOGGLE) { /* a switch: green when on, the knob moves */
        bool on = r->get && r->get(r->ctx);
        snprintf(v, sizeof(v), "%d", on);
        if (!strcmp(v, A.ptext[i])) return;
        nema_strlcpy(A.ptext[i], v, sizeof(A.ptext[i]));
        lv_obj_set_style_bg_color(A.pval[i], on ? C_GREEN : C_TRACK, 0);
        lv_obj_t *knob = lv_obj_get_child(A.pval[i], 0);
        lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
        return;
    }
    pal_menu_value_text(r, v, sizeof(v));
    if (r->kind == PAL_MENU_SUBMENU || (r->kind == PAL_MENU_CHOICE)) {
        size_t n = strlen(v);
        snprintf(v + n, sizeof(v) - n, "%s%s", n ? "  " : "", LV_SYMBOL_RIGHT);
    }
    if (!strcmp(v, A.ptext[i])) return;
    nema_strlcpy(A.ptext[i], v, sizeof(A.ptext[i]));
    lv_label_set_text(A.pval[i], v);
}

static void page_style_row(int i) {
    const pal_menu_row_t *r = &A.ppage->rows[A.pidx[i]];
    if (r->kind == PAL_MENU_SECTION) return;
    bool on = A.pidx[i] == A.psel;
    lv_obj_set_style_bg_color(A.prow[i], on ? C_BLUE : C_ROW, 0);
    lv_obj_t *label = lv_obj_get_child(A.prow[i], 0);
    lv_color_t ink = r->danger ? C_RED : r->disabled ? C_GREY : C_INK;
    lv_obj_set_style_text_color(label, on ? C_WHITE : ink, 0);
    if (A.pval[i] && r->kind != PAL_MENU_TOGGLE) lv_obj_set_style_text_color(A.pval[i], on ? C_WHITE : C_GREY, 0);
}

static void page_build(void) {
    lv_obj_clean(A.page_list);
    A.pn = 0;
    A.psel = A.ppage->sel;
    A.prows = A.ppage->n;
    const char *title = A.ppage->title && A.ppage->title[0] ? A.ppage->title : NULL;
    if (!title) { /* a root page without a title: the app's name */
        pal_view_t *fv = pal_desktop_view(pal_desktop_front_id());
        title = fv && fv->title ? fv->title : "";
    }
    lv_label_set_text_fmt(A.page_title, "%s%s", pal_menu_depth(A.pm) > 1 ? LV_SYMBOL_LEFT "  " : "", title);
    /* inset rounded rows with gaps (Cupertino), or rows edge to edge with no gap (Material) */
    int inset = ath_T->flat ? 0 : 10, y = 2, w = A.w - 2 * inset, rh = ath_T->flat ? 30 : 26, step = ath_T->flat ? 30 : 30;
    for (size_t k = 0; k < A.ppage->n && A.pn < MAX_PAGE_ROWS; k++) {
        const pal_menu_row_t *r = &A.ppage->rows[k];
        if (pal_menu_row_hidden(r)) continue;
        int i = A.pn++;
        A.pidx[i] = (int)k;
        A.pval[i] = NULL;
        A.ptext[i][0] = 0;
        lv_obj_t *row = lv_obj_create(A.page_list);
        lv_obj_remove_style_all(row);
        A.prow[i] = row;
        if (r->kind == PAL_MENU_SECTION) { /* a small grey header above its group */
            lv_obj_set_size(row, w, 18);
            lv_obj_set_pos(row, inset, y + 2);
            lv_obj_t *l = ath_text(row, &lv_font_montserrat_12, C_GREY);
            lv_label_set_text(l, r->label ? r->label : "");
            lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 6, 0);
            y += 22;
            continue;
        }
        lv_obj_set_size(row, w, rh);
        lv_obj_set_pos(row, inset, y);
        lv_obj_set_style_radius(row, ath_T->r_row, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_t *l = ath_text(row, &lv_font_montserrat_16, C_INK);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, w / 2 + 30);
        lv_label_set_text(l, r->label ? r->label : "");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        if (r->kind == PAL_MENU_TOGGLE) {
            lv_obj_t *sw = lv_obj_create(row);
            lv_obj_remove_style_all(sw);
            lv_obj_set_size(sw, 34, 18);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_t *knob = lv_obj_create(sw);
            lv_obj_remove_style_all(knob);
            lv_obj_set_size(knob, 14, 14);
            lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(knob, C_WHITE, 0);
            lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
            A.pval[i] = sw;
        } else {
            lv_obj_t *v = ath_text(row, &lv_font_montserrat_12, C_GREY);
            lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_width(v, w / 2 - 40);
            lv_label_set_text(v, "");
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
            A.pval[i] = v;
        }
        page_value(i);
        page_style_row(i);
        y += step;
    }
    if (A.pn == 0) lv_label_set_text(A.page_empty, A.ppage->empty ? A.ppage->empty : "Nothing here");
    else lv_label_set_text(A.page_empty, "");
    for (int i = 0; i < A.pn; i++)
        if (A.pidx[i] == A.psel) lv_obj_scroll_to_view(A.prow[i], LV_ANIM_OFF);
}

/* Follows the bound menu: a new page or row count rebuilds, a new selection restyles, values refresh twice a second. */
void ath_page_sync(pal_menu_t *m, uint32_t now) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p) return;
    if (m != A.pm || p != A.ppage || p->n != A.prows) {
        A.pm = m;
        A.ppage = p;
        page_build();
        A.pvals_ms = now;
        return;
    }
    if (p->sel != A.psel) {
        int old = A.psel;
        A.psel = p->sel;
        for (int i = 0; i < A.pn; i++)
            if (A.pidx[i] == old || A.pidx[i] == A.psel) {
                page_style_row(i);
                if (A.pidx[i] == A.psel) lv_obj_scroll_to_view(A.prow[i], LV_ANIM_OFF);
            }
    }
    if (now - A.pvals_ms >= 500u) {
        A.pvals_ms = now;
        for (int i = 0; i < A.pn; i++) page_value(i);
    }
    (void)selectable;
}

void ath_build_page(void) {
    A.page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(A.page);
    lv_obj_set_pos(A.page, 0, BAR_H);
    lv_obj_set_size(A.page, A.w, A.h - BAR_H);
    lv_obj_set_style_bg_color(A.page, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(A.page, LV_OBJ_FLAG_SCROLLABLE);
    A.page_title = ath_text(A.page, &lv_font_montserrat_20, C_INK);
    lv_obj_set_pos(A.page_title, 14, 4);
    A.page_list = lv_obj_create(A.page);
    lv_obj_remove_style_all(A.page_list);
    lv_obj_set_pos(A.page_list, 0, 32);
    lv_obj_set_size(A.page_list, A.w, A.h - BAR_H - 32);
    lv_obj_add_flag(A.page_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(A.page_list, LV_SCROLLBAR_MODE_OFF);
    A.page_empty = ath_text(A.page, &lv_font_montserrat_16, C_GREY);
    lv_label_set_text(A.page_empty, "");
    lv_obj_align(A.page_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(A.page, LV_OBJ_FLAG_HIDDEN);
}

