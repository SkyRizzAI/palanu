/* kui app list: the reference AppListScreen on the logical 1-bit canvas.
 * A VirtualList of ListItemRow rows: each row is 15 px high (the REG8 cell 11 + 4, the selected box in
 * apps-list.png) with the 2 px gap below it, a 17 px pitch, inset 2 px from the list edges. Inside a row the children are [3 px inset] [icon] [3 px] [label] [chevron] [5 px inset] with the flex gap of
 * 2 px between them, so the icon sits at x+5, the label at x+12+icon width, and the chevron 7 px from the right edge. The
 * selected row is inverted by kui_row_frame (the selectBox look). */
#include <string.h>

#include "pal/kui.h"

#define ROW_H 15
#define SLOT_H 17 /* ROW_H plus the 2 px gap */
#define PAD 2     /* the list's inset */
#define BAR_X_INSET 3

static bool src_bit(const uint8_t *bits, int sw, int sx, int sy) {
    int bpr = (sw + 7) / 8;
    return (bits[sy * bpr + (sx >> 3)] >> (7 - (sx & 7))) & 1;
}

/* The label, truncated with "..." to fit avail pixels (the reference label truncation in ListItemRow). */
static void draw_label(kui_canvas_t *c, int x, int y, int avail, const char *s, bool on) {
    char buf[64];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    if (kui_text_w(&FONT_REG8, buf) > avail) {
        int dots = kui_text_w(&FONT_REG8, "...");
        while (n > 0 && kui_text_w(&FONT_REG8, buf) + dots > avail) buf[--n] = 0;
        memcpy(buf + n, "...", 4);
    }
    kui_text(c, &FONT_REG8, x, y, buf, on);
}

static void row(kui_canvas_t *c, int x, int y, int w, const kui_row_t *r, bool sel) {
    kui_row_frame(c, x, y, w, ROW_H, sel);
    int tx = x + 3 + 2; /* the label's x when there is no icon: 3 px inset + gap */
    if (r->bits && r->w > 0 && r->h > 0) {
        int iw = r->w <= 8 ? r->w : 8, ih = r->h <= 8 ? r->h : 8;
        int ix = x + 5, iy = y + (ROW_H - ih) / 2;
        kui_bitmap_scaled(c, r->bits, r->w, r->h, ix, iy, iw, ih, !sel);
        tx = ix + iw + 2 + 3 + 2;
    }
    int right = x + w - 5 - 2; /* the chevron's cell ends 2 px before the right inset */
    int cw = r->chevron ? kui_text_w(&FONT_REG8, ">") : 0;
    int avail = (r->chevron ? right - cw - 2 : x + w - 5) - tx;
    if (avail < 0) avail = 0;
    draw_label(c, tx, y + (ROW_H - FONT_REG8.charH) / 2, avail, r->label ? r->label : "", !sel);
    if (r->chevron) kui_text(c, &FONT_REG8, right - cw, y + (ROW_H - FONT_REG8.charH) / 2, ">", !sel);
}

void kui_applist(kui_canvas_t *c, const kui_row_t *rows, int n, int cursor, int *first) {
    if (n <= 0 || !first) return;
    const int W = c->w, H = c->h, top = c->clip_y0;
    if (cursor < 0) cursor = 0;
    if (cursor >= n) cursor = n - 1;
    int area = H - top - 2 * PAD;
    int vis = area / SLOT_H;
    if (vis < 1) vis = 1;
    if (*first > cursor) *first = cursor;
    if (cursor >= *first + vis) *first = cursor - vis + 1;
    if (n > vis && *first > n - vis) *first = n - vis;
    if (*first < 0 || n <= vis) *first = 0;

    bool bar = n > vis;
    int rw = W - 2 * PAD - (bar ? BAR_X_INSET : 0);
    int y = top + PAD;
    for (int i = *first; i < *first + vis && i < n; i++, y += SLOT_H) row(c, PAD, y, rw, &rows[i], i == cursor);
    if (bar) kui_scrollbar(c, W - BAR_X_INSET, top + PAD, area, *first * SLOT_H, vis * SLOT_H, n * SLOT_H - 2);
}
