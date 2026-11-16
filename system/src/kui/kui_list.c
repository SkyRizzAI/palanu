/* kui list rows: the reference ListItemRow, ListInputRow, Switch and scrollbar. Coordinates are logical. Children are placed with the same inset and gap rules the reference flex
 * layout gives them: a 3 px pad plus the 2 px row gap puts the label at x+5, and the right inset is 5 px plus the gap. */
#include "pal/kui.h"

#define ROW_GAP 2
#define LABEL_X 5
#define RIGHT_INSET 7 /* 5 px inset and the 2 px gap before it */
#define SWITCH_W 18
#define SWITCH_H 9
#define CHEV_W 8

/* The reference "label ... " truncation: the longest prefix that fits with "..." after it. */
static int fit_text(const kui_font_t *f, const char *s, int avail, char *out, int cap) {
    int w = kui_text_w(f, s);
    if (w <= avail) {
        int n = 0;
        for (; s[n] && n + 1 < cap; n++) out[n] = s[n];
        out[n] = 0;
        return w;
    }
    int n = 0, best = 0;
    for (; s[n] && n + 1 < cap - 3; n++) {
        char tmp[64];
        int k = n + 1 < (int)sizeof(tmp) - 4 ? n + 1 : (int)sizeof(tmp) - 4;
        for (int i = 0; i < k; i++) tmp[i] = s[i];
        tmp[k] = 0;
        int cand = kui_text_w(f, tmp) + kui_text_w(f, "...");
        if (cand > avail) break;
        best = k;
    }
    int i = 0;
    for (; i < best; i++) out[i] = s[i];
    out[i++] = '.';
    out[i++] = '.';
    out[i++] = '.';
    out[i] = 0;
    return kui_text_w(f, out);
}

void kui_row_frame(kui_canvas_t *c, int x, int y, int w, int h, bool selected) {
    if (!selected || w < 10 || h < 3) return;
    /* The reference highlightBox, selectBox branch: the row's middle band and both caps are XOR-inverted; the corners keep paper. */
    kui_invert(c, x, y + 1, w, h - 2);
    kui_invert(c, x + 1, y, w - 2, 1);
    kui_invert(c, x + 1, y + h - 1, w - 2, 1);
}

void kui_row_label(kui_canvas_t *c, int x, int y, int h, int avail, const char *label) {
    char shown[96];
    fit_text(kui_slot(KUI_F_REG8), label, avail, shown, sizeof(shown));
    kui_text(c, kui_slot(KUI_F_REG8), x + LABEL_X, y + (h - kui_slot(KUI_F_REG8)->charH) / 2, shown, true);
}

void kui_row_chevron(kui_canvas_t *c, int right, int y, int h, bool next) {
    /* '>' hugs the right inset; '<' is the same glyph mirrored text, placed right of its cell. */
    int cw = kui_text_w(kui_slot(KUI_F_REG8), ">");
    kui_text(c, kui_slot(KUI_F_REG8), right - cw, y + (h - kui_slot(KUI_F_REG8)->charH) / 2, next ? ">" : "<", true);
}

/* The reference Switch: on = filled track with the knob punched out on the right; off = outline track, filled knob on the left. */
void kui_switch(kui_canvas_t *c, int x, int y, bool on) {
    const int r = 3, kd = SWITCH_H - 4, kr = kd / 2;
    if (on) {
        kui_fill_round(c, x, y, SWITCH_W, SWITCH_H, r, true);
        kui_fill_round(c, x + SWITCH_W - 2 - kd, y + 2, kd, kd, kr, false);
    } else {
        kui_round(c, x, y, SWITCH_W, SWITCH_H, r, true);
        kui_fill_round(c, x + 2, y + 2, kd, kd, kr, true);
    }
}

void kui_row_switch(kui_canvas_t *c, int x, int y, int h, int w, bool on) {
    kui_switch(c, x + w - RIGHT_INSET - SWITCH_W, y + (h - SWITCH_H) / 2, on);
}

/* The reference draws the value of a choice row 1 px further right than the centred column (measured against the reference shots). */
#define KUI_CHOICE_VALUE_DX 1

/* The reference ListInputRow: [5] label (11 parts) | '<' cell 8 | value (9 parts) | '>' cell 8 | [5]; flex parts split the rest. */
void kui_row_choice(kui_canvas_t *c, int x, int y, int h, int w, const char *label, const char *value, bool can_prev,
                    bool can_next) {
    int rem = w - 2 * LABEL_X - 2 * CHEV_W;
    int lw = rem * 11 / 20, vw = rem * 9 / 20;
    int lx = x + LABEL_X, cx = lx + lw, vx = cx + CHEV_W, rx = vx + vw;
    kui_row_label(c, x, y, h, lw, label);
    if (can_prev) kui_row_chevron(c, cx + CHEV_W, y, h, false);
    int vwidth = kui_text_w(kui_slot(KUI_F_REG8), value);
    kui_text(c, kui_slot(KUI_F_REG8), vx + (vw - vwidth) / 2 + KUI_CHOICE_VALUE_DX, y + (h - kui_slot(KUI_F_REG8)->charH) / 2, value, true);
    if (can_next) kui_text(c, kui_slot(KUI_F_REG8), rx + KUI_CHOICE_VALUE_DX, y + (h - kui_slot(KUI_F_REG8)->charH) / 2, ">", true);
}

/* The reference scrollbar: a dashed track (1 px every 2 px) and a solid 3 px thumb. */
void kui_scrollbar(kui_canvas_t *c, int x, int y, int size, int offset, int viewport, int content) {
    const int bar = 3, min_thumb = 4;
    if (size <= 0) return;
    for (int i = 0; i < size; i += 2) kui_fill(c, x + 1, y + i, 1, 1, true);
    if (content <= viewport || viewport <= 0) {
        kui_fill(c, x, y, bar, size, true);
        return;
    }
    int thumb = size * viewport / content;
    if (thumb < min_thumb) thumb = min_thumb;
    if (thumb > size) thumb = size;
    int max_scroll = content - viewport;
    int off = offset > max_scroll ? max_scroll : offset;
    int travel = size - thumb;
    int thumb_off = max_scroll ? off * travel / max_scroll : 0;
    kui_fill(c, x, y + thumb_off, bar, thumb, true);
}
