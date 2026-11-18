/* Athena draws the shell services (pal/shell.h) and the toasts itself (ADR-0010 P3). A service in front becomes a modal: the screen
 * under it (copied from the system frame when it opens, whatever drew it: the home, a page or an app) dimmed, and a rounded card on
 * top. The dialog has two buttons, the picker a list, the text entry a field with a caret, the busy box a turning glyph, and the
 * on-screen keyboard a sheet of keys at the bottom. The keys still go to the service (the compositor gives them to the top overlay),
 * so the behaviour is the system's. A toast shows in the status bar, over the clock, for as long as the system keeps it. */
#include <stdio.h>
#include <string.h>

#include "athena_int.h"
#include "pal/shell.h"

static lv_image_dsc_t s_snap_dsc;

/* A short signature of what the modal shows: it is rebuilt only when this changes. */
static uint32_t mix(uint32_t h, const char *s) {
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return (h ^ 0xFFu) * 16777619u;
}

static uint32_t signature(const pal_shell_view_t *v) {
    uint32_t h = 2166136261u ^ (uint32_t)v->kind;
    h = mix(h, v->title), h = mix(h, v->text);
    for (int i = 0; i < v->nitems; i++) h = mix(h, v->items[i]);
    for (int i = 0; i < v->nkeys; i++) h = mix(h, v->keys[i].label);
    h = (h ^ (uint32_t)v->sel) * 16777619u;
    h = (h ^ (uint32_t)v->cursor) * 16777619u;
    h = (h ^ (uint32_t)v->focus) * 16777619u;
    return h;
}

void ath_shell_build(void) {
    A.backdrop = lv_image_create(lv_layer_top());
    lv_obj_set_pos(A.backdrop, 0, 0);
    lv_obj_add_flag(A.backdrop, LV_OBJ_FLAG_HIDDEN);
    A.dim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(A.dim);
    lv_obj_set_size(A.dim, A.w, A.h);
    lv_obj_set_style_bg_color(A.dim, C_BLACK, 0);
    lv_obj_set_style_bg_opa(A.dim, LV_OPA_50, 0);
    lv_obj_add_flag(A.dim, LV_OBJ_FLAG_HIDDEN);
    A.modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(A.modal);
    lv_obj_set_style_radius(A.modal, 14, 0);
    lv_obj_set_style_bg_color(A.modal, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.modal, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(A.modal, 16, 0);
    lv_obj_set_style_shadow_opa(A.modal, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(A.modal, C_BLACK, 0);
    lv_obj_remove_flag(A.modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(A.modal, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h, bool on, const char *label, bool accent) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, on ? C_BLUE : accent ? lv_color_hex(0xD1D1D6) : C_ROW, 0);
    lv_obj_t *l = ath_text(b, &lv_font_montserrat_12, on ? C_WHITE : C_INK);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    return b;
}

/* The entry with a caret, as one line that keeps the caret in view. */
static void field(lv_obj_t *parent, int x, int y, int w, const char *text, int cursor) {
    lv_obj_t *f = lv_obj_create(parent);
    lv_obj_remove_style_all(f);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_size(f, w, 26);
    lv_obj_set_style_radius(f, 8, 0);
    lv_obj_set_style_bg_color(f, C_ROW, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(f, C_BLUE, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    char shown[96];
    const char *t = text ? text : "";
    int len = (int)strlen(t), c = cursor < 0 ? 0 : cursor > len ? len : cursor, room = (w - 16) / 7;
    int from = c > room ? c - room : 0;
    snprintf(shown, sizeof(shown), "%.*s|%.*s", c - from, t + from, room, t + c);
    lv_obj_t *l = ath_text(f, &lv_font_montserrat_12, C_INK);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w - 16);
    lv_label_set_text(l, shown);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
}

static void card(int w, int h, bool bottom) {
    lv_obj_set_size(A.modal, w, h);
    if (bottom) lv_obj_align(A.modal, LV_ALIGN_BOTTOM_MID, 0, -4);
    else lv_obj_align(A.modal, LV_ALIGN_CENTER, 0, BAR_H / 2);
}

static lv_obj_t *title(const char *t, int y) {
    lv_obj_t *l = ath_text(A.modal, &lv_font_montserrat_16, C_INK);
    lv_label_set_text(l, t ? t : "");
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static void build_dialog(const pal_shell_view_t *v) {
    int w = A.w - 60;
    card(w, 118, false);
    title(v->title, 12);
    lv_obj_t *body = ath_text(A.modal, &lv_font_montserrat_12, C_GREY);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, w - 28);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(body, v->text ? v->text : "");
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 36);
    int bw = (w - 36) / 2;
    pill(A.modal, 12, 118 - 38, bw, 28, v->sel == 0, v->items[0], true);
    pill(A.modal, 24 + bw, 118 - 38, bw, 28, v->sel == 1, v->items[1], false);
}

static void build_picker(const pal_shell_view_t *v) {
    int w = A.w - 60, vis = v->nitems < 5 ? v->nitems : 5, h = 44 + vis * 28 + 8;
    card(w, h, false);
    title(v->title, 10);
    int first = v->sel >= vis ? v->sel - vis + 1 : 0;
    for (int i = 0; i < vis; i++) {
        int k = first + i;
        lv_obj_t *r = pill(A.modal, 10, 40 + i * 28, w - 20, 24, k == v->sel, v->items[k], false);
        lv_obj_t *l = lv_obj_get_child(r, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
    }
}

static void build_text(const pal_shell_view_t *v) {
    int w = A.w - 40;
    card(w, 96, false);
    title(v->title, 10);
    field(A.modal, 12, 38, w - 24, v->text, v->cursor);
    lv_obj_t *hint = ath_text(A.modal, &lv_font_montserrat_12, C_GREY);
    lv_label_set_text(hint, "Enter done  |  Esc cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_busy(const pal_shell_view_t *v) {
    card(180, 70, false);
    static const char *const SPIN[] = {LV_SYMBOL_REFRESH, LV_SYMBOL_LOOP};
    lv_obj_t *g = ath_text(A.modal, &lv_font_montserrat_20, C_BLUE);
    lv_label_set_text(g, SPIN[(v->sel / 4) & 1]);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *l = ath_text(A.modal, &lv_font_montserrat_12, C_INK);
    lv_label_set_text(l, v->text ? v->text : "");
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* The keys in their rows: 20 half-units across, 4 rows, each key a rounded cap; the focused one blue. */
static void build_keyboard(const pal_shell_view_t *v) {
    int w = A.w - 8, kh = 26, gap = 3, top = 52, h = top + v->rows * (kh + gap) + 6;
    card(w, h, true);
    lv_obj_t *t = ath_text(A.modal, &lv_font_montserrat_12, C_GREY);
    lv_label_set_text(t, v->title && v->title[0] ? v->title : "Keyboard");
    lv_obj_set_pos(t, 10, 6);
    field(A.modal, 8, 22, w - 16, v->text, v->cursor);
    float hu = (float)(w - 12) / 20.0f;
    for (int i = 0; i < v->nkeys; i++) {
        const pal_shell_key_t *k = &v->keys[i];
        int x = 6 + (int)((float)k->x * hu) + 1, kw = (int)((float)k->w * hu) - 2;
        const char *label = k->label;
        if (k->accent && (!strcmp(label, "v") || !strcmp(label, "^"))) label = LV_SYMBOL_UP; /* shift: off or latched */
        else if (!strcmp(label, "<-")) label = LV_SYMBOL_BACKSPACE;
        else if (!strcmp(label, "ENTER")) label = LV_SYMBOL_NEW_LINE;
        lv_obj_t *cap = pill(A.modal, x, top + k->row * (kh + gap), kw, kh, i == v->focus, label, k->accent);
        if (k->accent && !strcmp(k->label, "^") && i != v->focus) lv_obj_set_style_bg_color(cap, lv_color_hex(0xB9D7FF), 0);
    }
}

/* The screen under the modal: a copy of the system frame, made when the modal opens. */
static void take_snapshot(void) {
    const uint16_t *f = pal_display_frame();
    if (!f || !A.snap) return;
    pal_display_frame_lock(true);
    memcpy(A.snap, f, (size_t)A.w * (size_t)A.h * 2u);
    pal_display_frame_lock(false);
    memset(&s_snap_dsc, 0, sizeof(s_snap_dsc));
    s_snap_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_snap_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_snap_dsc.header.w = (uint32_t)A.w;
    s_snap_dsc.header.h = (uint32_t)A.h;
    s_snap_dsc.header.stride = (uint32_t)A.w * 2u;
    s_snap_dsc.data = (const uint8_t *)A.snap;
    s_snap_dsc.data_size = (uint32_t)A.w * (uint32_t)A.h * 2u;
    lv_image_set_src(A.backdrop, &s_snap_dsc);
}

bool ath_shell_active(void) { return A.svc_kind != PAL_SHELL_NONE; }

static void toast_sync(void) {
    bool on = pal_toast_showing();
    if (on == A.toast_shown && !on) return;
    if (on) {
        const char *t = pal_toast_text();
        if (!A.toast_shown || strcmp(lv_label_get_text(A.bar_toast), t) != 0) lv_label_set_text(A.bar_toast, t);
    }
    if (on != A.toast_shown) {
        A.toast_shown = on;
        if (on) {
            lv_obj_remove_flag(A.bar_toast, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(A.bar_time, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(A.bar_right, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(A.bar_toast, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(A.bar_time, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(A.bar_right, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ath_shell_sync(void) {
    toast_sync();
    pal_shell_view_t v;
    bool on = !pal_desktop_locked() && pal_shell_top(&v);
    if (!on) {
        if (A.svc_kind != PAL_SHELL_NONE) {
            A.svc_kind = PAL_SHELL_NONE;
            A.svc_sig = 0;
            lv_obj_add_flag(A.backdrop, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(A.dim, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(A.modal, LV_OBJ_FLAG_HIDDEN);
            A.force = true;
        }
        return;
    }
    if (A.svc_kind == PAL_SHELL_NONE) { /* it opens: keep what is on screen under it */
        take_snapshot();
        lv_obj_remove_flag(A.backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(A.dim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(A.modal, LV_OBJ_FLAG_HIDDEN);
        A.force = true;
    }
    A.svc_kind = (int)v.kind;
    uint32_t sig = signature(&v);
    if (sig == A.svc_sig) return;
    A.svc_sig = sig;
    lv_obj_clean(A.modal);
    switch (v.kind) {
    case PAL_SHELL_DIALOG: build_dialog(&v); break;
    case PAL_SHELL_PICKER: build_picker(&v); break;
    case PAL_SHELL_TEXT: build_text(&v); break;
    case PAL_SHELL_BUSY: build_busy(&v); break;
    case PAL_SHELL_KEYBOARD: build_keyboard(&v); break;
    default: break;
    }
}
