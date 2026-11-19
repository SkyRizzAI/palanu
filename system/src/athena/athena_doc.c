/* Athena: a document screen (pal/doc.h: a log, a file) drawn in Athena's look. The app keeps its state and keys; this file reads
 * the bound doc each frame and shows the title (a navigation title or a pane, by theme), the toolbar rows (a switch or a value,
 * the focused one on the highlight) and the lines on a card, fixed-width for a file. It writes back how many lines fit, so the
 * app's paging follows this look. */
#include "athena_int.h"
#include "pal/doc.h"
#include "pal/shell.h"

#define TOOL_H 26

pal_view_t *ath_app_view(void) {
    for (int i = ovl_count() - 1; i >= 0; i--) {
        pal_view_t *v = ovl_view(i);
        if (v && !v->describe) return v; /* a shell service describes itself; the app's own overlay does not */
    }
    return pal_desktop_view(pal_desktop_front_id());
}

pal_doc_t *ath_doc(void) {
    if (pal_desktop_locked()) return NULL;
    pal_view_t *v = ath_app_view();
    return v ? pal_doc_bound(v->id) : NULL;
}

bool ath_send_key(pal_keysym_t sym) {
    pal_view_t *v = ath_app_view();
    if (!v || !v->key) return false;
    pal_key_t k = {.sym = (uint8_t)sym};
    bool used = v->key(v, &k);
    if (used) pal_desktop_invalidate();
    return used;
}

static int line_h(const pal_doc_t *d) { return d && d->mono ? 10 : 15; }

/* The objects for this doc: the toolbar rows it has now and as many line labels as fit below them. */
static void doc_layout(pal_doc_t *d) {
    lv_obj_clean(A.doc_body);
    int nt = d->ntools ? d->ntools(d->ctx) : 0;
    if (nt > MAX_DOC_TOOLS) nt = MAX_DOC_TOOLS;
    A.dtools = nt;
    bool grouped = !ath_T->flat;
    int inset = grouped ? 10 : 6, w = A.w - 2 * inset, y = 4;
    int body_h = A.h - BAR_H - ath_bottom() - lv_obj_get_y(A.doc_body);
    for (int i = 0; i < nt; i++) {
        lv_obj_t *r = lv_obj_create(A.doc_body);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, w, TOOL_H);
        lv_obj_set_pos(r, inset, y);
        lv_obj_set_style_radius(r, ath_T->r_row, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        A.doc_tool[i] = r;
        A.doc_tlabel[i] = ath_text(r, &lv_font_montserrat_16, C_INK);
        lv_label_set_text(A.doc_tlabel[i], d->tools[i].label);
        lv_obj_align(A.doc_tlabel[i], LV_ALIGN_LEFT_MID, 10, 0);
        if (d->tools[i].get) { /* a switch */
            lv_obj_t *sw = lv_obj_create(r);
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
            A.doc_tval[i] = sw;
        } else {
            A.doc_tval[i] = ath_text(r, &lv_font_montserrat_12, C_GREY);
            lv_label_set_text(A.doc_tval[i], "");
            lv_obj_align(A.doc_tval[i], LV_ALIGN_RIGHT_MID, -10, 0);
        }
        y += TOOL_H + 3;
    }
    /* the lines on a card */
    lv_obj_t *card = lv_obj_create(A.doc_body);
    lv_obj_remove_style_all(card);
    int ch = body_h - y - 4;
    lv_obj_set_size(card, w, ch);
    lv_obj_set_pos(card, inset, y + 1);
    lv_obj_set_style_radius(card, ath_T->r_card, 0);
    lv_obj_set_style_bg_color(card, d->mono ? lv_color_hex(0x15181D) : C_ROW, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    int lh = line_h(d);
    A.dvis = (ch - 8) / lh;
    if (A.dvis > MAX_DOC_LINES) A.dvis = MAX_DOC_LINES;
    if (A.dvis < 1) A.dvis = 1;
    for (int i = 0; i < A.dvis; i++) {
        lv_obj_t *l = ath_text(card, d->mono ? &lv_font_unscii_8 : &lv_font_montserrat_12, d->mono ? lv_color_hex(0xE4E8EE) : C_INK);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(l, w - 10, lh);
        lv_obj_set_pos(l, 5, 4 + i * lh);
        lv_obj_set_style_radius(l, 2, 0);
        lv_obj_set_style_bg_color(l, lv_color_hex(0xFFE27A), 0); /* a search hit */
        lv_label_set_text(l, "");
        A.doc_line[i] = l;
    }
    A.doc_empty = ath_text(card, &lv_font_montserrat_16, d->mono ? lv_color_hex(0x9AA3AE) : C_GREY);
    lv_obj_align(A.doc_empty, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(A.doc_empty, "");
    if (d->vis && *d->vis != A.dvis) { /* the app pages by what this look shows */
        *d->vis = A.dvis;
        if (d->relayout) d->relayout(d->ctx);
    }
}

static void doc_fill(pal_doc_t *d) {
    char t[PAL_DOC_LINE_MAX];
    t[0] = 0;
    if (d->title) d->title(d->ctx, t, sizeof(t));
    else {
        pal_view_t *v = ath_app_view();
        nema_strlcpy(t, v && v->title ? v->title : "", sizeof(t));
    }
    if (strcmp(lv_label_get_text(A.doc_title), t)) lv_label_set_text(A.doc_title, t);
    int focus = d->focus ? *d->focus : -1;
    for (int i = 0; i < A.dtools; i++) {
        bool on = i == focus;
        ath_focus_bg(A.doc_tool[i], on, C_ROW);
        lv_obj_set_style_text_color(A.doc_tlabel[i], on ? C_WHITE : C_INK, 0);
        if (d->tools[i].get) {
            bool v = d->tools[i].get(d->ctx) != 0;
            lv_obj_set_style_bg_color(A.doc_tval[i], v ? C_GREEN : C_TRACK, 0);
            lv_obj_align(lv_obj_get_child(A.doc_tval[i], 0), v ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, v ? -2 : 2, 0);
        } else {
            char v[VALUE_MAX] = "";
            if (d->tools[i].value) {
                d->tools[i].value(d->ctx, v, sizeof(v));
                size_t n = strlen(v);
                snprintf(v + n, sizeof(v) - n, "  " LV_SYMBOL_RIGHT);
            }
            lv_label_set_text(A.doc_tval[i], v);
            lv_obj_set_style_text_color(A.doc_tval[i], on ? C_WHITE : C_GREY, 0);
        }
    }
    int n = d->count ? d->count(d->ctx) : 0, top = d->top ? *d->top : 0;
    int max = n - A.dvis;
    if (top > max) top = max;
    if (top < 0) top = 0;
    for (int i = 0; i < A.dvis; i++) {
        bool mark = false;
        t[0] = 0;
        if (top + i < n) d->line(d->ctx, top + i, t, sizeof(t), &mark);
        lv_label_set_text(A.doc_line[i], t);
        lv_obj_set_style_bg_opa(A.doc_line[i], mark ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(A.doc_line[i], mark ? C_BLACK : d->mono ? lv_color_hex(0xE4E8EE) : C_INK, 0);
    }
    t[0] = 0;
    if (n == 0 && d->empty) d->empty(d->ctx, t, sizeof(t));
    lv_label_set_text(A.doc_empty, t);
}

/* Follows the doc: another doc or toolbar lays the screen out again; a change of its lines, first line, focus or count refills it,
 * and the toolbar's values refresh twice a second. */
void ath_doc_sync(pal_doc_t *d, uint32_t now) {
    int nt = d->ntools ? d->ntools(d->ctx) : 0;
    if (nt > MAX_DOC_TOOLS) nt = MAX_DOC_TOOLS;
    if (d != A.dm || nt != A.dtools) {
        A.dm = d;
        doc_layout(d);
        A.dsig = ~0u;
    }
    if (d->vis && *d->vis != A.dvis) { /* the classic look drew in between and wrote its own */
        *d->vis = A.dvis;
        if (d->relayout) d->relayout(d->ctx);
    }
    int n = d->count ? d->count(d->ctx) : 0;
    uint32_t sig = d->gen * 2654435761u ^ (uint32_t)(d->top ? *d->top : 0) * 40503u ^ (uint32_t)(d->focus ? *d->focus : -1) * 97u ^
                   (uint32_t)n * 13u;
    if (sig != A.dsig || now - A.dvals_ms >= 500u) {
        A.dsig = sig;
        A.dvals_ms = now;
        doc_fill(d);
    }
}

void ath_build_doc(void) {
    A.doc = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(A.doc);
    lv_obj_set_pos(A.doc, 0, BAR_H);
    lv_obj_set_size(A.doc, A.w, A.h - BAR_H - ath_bottom());
    lv_obj_set_style_bg_color(A.doc, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.doc, LV_OPA_COVER, 0);
    lv_obj_remove_flag(A.doc, LV_OBJ_FLAG_SCROLLABLE);
    int body_y = 30;
    A.doc_pane = NULL;
    if (ath_T->title_pane) {
        body_y = 24;
        A.doc_pane = lv_obj_create(A.doc);
        lv_obj_remove_style_all(A.doc_pane);
        lv_obj_set_size(A.doc_pane, A.w, 24);
        ath_pane_bg(A.doc_pane);
        lv_obj_remove_flag(A.doc_pane, LV_OBJ_FLAG_SCROLLABLE);
        A.doc_title = ath_text(A.doc_pane, &lv_font_montserrat_16, C_WHITE);
    } else {
        A.doc_title = ath_text(A.doc, ath_T->flat ? &lv_font_montserrat_20 : &lv_font_montserrat_16, C_INK);
    }
    lv_label_set_long_mode(A.doc_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(A.doc_title, A.w - 20, ath_T->flat && !ath_T->title_pane ? 24 : 19); /* one line: a long name ends in dots */
    lv_obj_set_style_text_align(A.doc_title, ath_T->title_left || ath_T->title_pane ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    if (A.doc_pane) lv_obj_align(A.doc_title, LV_ALIGN_LEFT_MID, 8, 0);
    else lv_obj_set_pos(A.doc_title, 10, ath_T->flat ? 3 : 6);
    lv_label_set_text(A.doc_title, "");
    A.doc_body = lv_obj_create(A.doc);
    lv_obj_remove_style_all(A.doc_body);
    lv_obj_set_pos(A.doc_body, 0, body_y);
    lv_obj_set_size(A.doc_body, A.w, A.h - BAR_H - ath_bottom() - body_y);
    lv_obj_remove_flag(A.doc_body, LV_OBJ_FLAG_SCROLLABLE);
    A.dm = NULL;
    lv_obj_add_flag(A.doc, LV_OBJ_FLAG_HIDDEN);
}
