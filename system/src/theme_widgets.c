/* Default dialog frame, text field, toast and spinner. The dialog and the toast are the reference (kui_overlay.c: the dimmed
 * backdrop, the box, the ink band); the text field, spinner and frame keep the pixel-art look of the rest of the defaults. A
 * theme that wants other shapes replaces the member. */
#include <string.h>

#include "pal/kui.h"
#include "theme_widgets.h"

pal_rect_t pal_def_dialog_box(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t area, const char *title, const char *text,
                              const char *const *buttons, int nbuttons, int selected) {
    (void)t;
    /* PalanuOS texts wrap; the box grows to hold them (kui_dialog, wrap). */
    return kui_dialog(c, area, title, text, buttons, nbuttons, selected, true);
}

int pal_def_text_field(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int w, const char *shown, int caret,
                       bool caret_on) {
    const pal_font_t *f = t->tokens.body;
    int h = f->h + 6;
    pal_round(c, x, y, w, h, t->tokens.radius, PAL_C_INK);
    int tx = x + t->tokens.pad_x;
    pal_text(c, f, tx, y + 3, shown, PAL_C_INK, PAL_TRANSPARENT);
    if (caret_on) {
        int cx = tx + caret * f->w;
        if (cx < x + w - 2) pal_fill(c, cx, y + 3, 2, f->h, PAL_C_INK);
    }
    return h;
}

void pal_def_toast(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t box, const char *text) {
    (void)t;
    kui_toast_band(c, box, text); /* The reference band; the caption in paper (see kui_toast_band) */
}

void pal_def_spinner(pal_canvas_t *c, const pal_theme_t *t, int cx, int cy, int frame) {
    static const int8_t DX[8] = {0, 3, 5, 3, 0, -3, -5, -3};
    static const int8_t DY[8] = {-5, -3, 0, 3, 5, 3, 0, -3};
    int lit = ((frame % 8) + 8) % 8;
    (void)t;
    for (int i = 0; i < 8; i++) {
        int d = (lit - i + 8) % 8;
        pal_color_t col = d == 0 ? PAL_C_INK : d == 1 ? PAL_C_DIM : PAL_C_PANEL;
        pal_fill(c, cx + DX[i] - 1, cy + DY[i] - 1, 2, 2, col);
    }
}

int pal_def_frame(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t box, const char *title) {
    const pal_font_t *ft = t->tokens.title;
    int r = t->tokens.radius;
    pal_fill_round(c, box.x, box.y, box.w, box.h, r, PAL_C_PAPER);
    pal_round(c, box.x, box.y, box.w, box.h, r, PAL_C_INK);
    if (!title || !title[0]) return 0;
    int th = ft->h + 6;
    pal_fill_round(c, box.x + 2, box.y + 2, box.w - 4, th, r, PAL_C_INK);
    int tx = box.x + (box.w - pal_text_w(ft, title)) / 2;
    if (tx < box.x + 4) tx = box.x + 4;
    pal_text(c, ft, tx, box.y + 5, title, PAL_C_PAPER, PAL_TRANSPARENT);
    return th + 2;
}

void pal_components_fill(pal_components_t *comp) {
    const pal_components_t *d = &PAL_DEFAULT_COMPONENTS;
    if (!comp->status_bar) comp->status_bar = d->status_bar;
    if (!comp->title_bar) comp->title_bar = d->title_bar;
    if (!comp->row_height) comp->row_height = d->row_height;
    if (!comp->list_row) comp->list_row = d->list_row;
    if (!comp->scrollbar) comp->scrollbar = d->scrollbar;
    if (!comp->dialog_box) comp->dialog_box = d->dialog_box;
    if (!comp->text_field) comp->text_field = d->text_field;
    if (!comp->toast) comp->toast = d->toast;
    if (!comp->spinner) comp->spinner = d->spinner;
}
