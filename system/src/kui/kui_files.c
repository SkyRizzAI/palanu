/* kui files and logs widgets: the reference ListSection, the 8x8 file icons (file.folder, file.file,
 * file.generic) and ListItemRow with an icon, a value and a chevron. Logical coordinates.
 * The row children are placed with the reference flex rules: a 3 px pad and the 2 px gap put the icon at x+5 and the label after it
 * at x+20 (or x+5 without an icon); the chevron and value sit 5 px in from the right edge plus the gap, with 2 px between them. */
#include "pal/kui.h"

#define ICON_W 8
#define LABEL_X 5        /* 3 px pad + the 2 px gap */
#define ICON_X 5
#define LABEL_AFTER_ICON 20 /* 5 + 8 icon + 2 gap + 3 pad + 2 gap */
#define RIGHT_EDGE 7     /* 5 px inset + the 2 px gap before it */
#define CHILD_GAP 2

static const uint8_t ICONS[3][ICON_W] = {
    {0x00, 0x70, 0xFF, 0x81, 0x81, 0x81, 0xFF, 0x00}, /* folder */
    {0x3E, 0x2A, 0x3E, 0x22, 0x22, 0x22, 0x3E, 0x00}, /* file */
    {0x7C, 0x44, 0x74, 0x44, 0x44, 0x44, 0x7C, 0x00}, /* generic */
};

void kui_icon8(kui_canvas_t *c, int x, int y, int icon, bool on) {
    if (icon < 0 || icon > KUI_ICON_GENERIC) return;
    kui_bitmap(c, x, y, ICON_W, ICON_W, ICONS[icon], on);
}

int kui_section(kui_canvas_t *c, int x, int y, const kui_font_t *f, const char *title) {
    kui_text(c, f, x + 5, y + 4, title, true);
    return 4 + f->charH + 1;
}

/* The longest prefix of s that fits avail pixels with "..." after it (the reference label truncation). */
static void fit_label(const kui_font_t *f, const char *s, int avail, char *out, int cap) {
    if (kui_text_w(f, s) <= avail) {
        int n = 0;
        for (; s[n] && n + 1 < cap; n++) out[n] = s[n];
        out[n] = 0;
        return;
    }
    int best = 0;
    char tmp[96];
    for (int n = 1; s[n - 1] && n + 3 < cap && n < (int)sizeof(tmp) - 4; n++) {
        for (int i = 0; i < n; i++) tmp[i] = s[i];
        tmp[n] = 0;
        int with = kui_text_w(f, tmp) + kui_text_w(f, "...");
        if (with > avail) break;
        best = n;
    }
    int i = 0;
    for (; i < best; i++) out[i] = s[i];
    out[i++] = '.';
    out[i++] = '.';
    out[i++] = '.';
    out[i] = 0;
}

void kui_list_item(kui_canvas_t *c, int x, int y, int w, const kui_list_item_t *it) {
    const int h = KUI_LIST_ROW_H;
    const int ty = y + (h - FONT_REG8.charH) / 2;
    int lx = x + LABEL_X;
    if (it->icon >= 0) {
        kui_icon8(c, x + ICON_X, y + (h - ICON_W) / 2, it->icon, true);
        lx = x + LABEL_AFTER_ICON;
    }
    int right = x + w - RIGHT_EDGE;
    if (it->chevron) {
        int cw = kui_text_w(&FONT_REG8, ">");
        kui_text(c, &FONT_REG8, right - cw, ty, ">", true);
        right -= cw + CHILD_GAP;
    }
    if (it->value && it->value[0]) {
        int vw = kui_text_w(&FONT_REG8, it->value);
        kui_text(c, &FONT_REG8, right - vw, ty, it->value, true);
        right -= vw + CHILD_GAP;
    }
    char shown[96];
    fit_label(&FONT_REG8, it->label ? it->label : "", right - lx, shown, sizeof(shown));
    kui_text(c, &FONT_REG8, lx, ty, shown, true);
    /* the focus box is inverted over the content (the reference selectBox), so it goes last */
    kui_row_frame(c, x, y, w, h, it->selected);
}

/* The reference ListInputRow: [5] label | '<' cell | value | '>' cell | [5], with the label at 11/20 of the space between the insets and
 * the chevron cells, and the value column taking the rest (the flex remainder, so the '>' sits where the reference puts it). */
void kui_list_choice(kui_canvas_t *c, int x, int y, int w, const char *label, const char *value, bool can_prev, bool can_next) {
    const int h = KUI_LIST_ROW_H;
    const int ty = y + (h - FONT_REG8.charH) / 2;
    int rem = w - 2 * LABEL_X - 2 * 8;
    int lw = rem * 11 / 20;
    int lx = x + LABEL_X, cx = lx + lw, vx = cx + 8, vw = rem - lw, rx = vx + vw;
    char shown[96];
    fit_label(&FONT_REG8, label ? label : "", lw - 2, shown, sizeof(shown));
    kui_text(c, &FONT_REG8, lx, ty, shown, true);
    if (can_prev) kui_text(c, &FONT_REG8, cx + 8 - kui_text_w(&FONT_REG8, "<"), ty, "<", true);
    const char *v = value ? value : "";
    kui_text(c, &FONT_REG8, vx + (vw - kui_text_w(&FONT_REG8, v)) / 2, ty, v, true);
    if (can_next) kui_text(c, &FONT_REG8, rx, ty, ">", true);
}

