/* kui canvas: the reference Canvas on a packed 1-bit logical buffer.
 * Every primitive keeps the reference arithmetic (chamfered rounds, Bresenham lines, run-length glyphs), so the logical pixels
 * are the same ones the reference driver expands. The only difference is storage: bits, not the driver's fillRect calls. */
#include <stdlib.h>
#include <string.h>

#include "pal/kui.h"
#include "pal/theme.h"

static int s_scale_q = 2 * KUI_Q; /* until the settings are read: the design scale */

int kui_scale_q(void) { return s_scale_q; }

void kui_set_scale_q(int q) { s_scale_q = q < KUI_SCALE_MIN_Q ? KUI_SCALE_MIN_Q : q > KUI_SCALE_MAX_Q ? KUI_SCALE_MAX_Q : q; }

int kui_top_px(int n) { return kui_ph(s_scale_q, n); }

int kui_bottom_px(int phys_h, int n) {
    int lh = phys_h * KUI_Q / s_scale_q; /* the canvas height in logical rows */
    if (lh > KUI_MAX_H) lh = KUI_MAX_H;
    return n >= lh ? phys_h : phys_h - kui_ph(s_scale_q, lh - n);
}

void kui_canvas_init(kui_canvas_t *c, int phys_w, int phys_h, int scale_q) {
    if (scale_q < KUI_SCALE_MIN_Q) scale_q = KUI_SCALE_MIN_Q;
    c->sq = scale_q;
    c->scale = (scale_q + KUI_Q / 2) / KUI_Q;
    c->w = phys_w * KUI_Q / scale_q;
    c->h = phys_h * KUI_Q / scale_q;
    if (c->w > KUI_MAX_W) c->w = KUI_MAX_W; /* the buffer holds the 1x canvas of a 320x240 panel */
    if (c->h > KUI_MAX_H) c->h = KUI_MAX_H;
    /* only the rows in use: a 2x canvas clears 2.4 KB, not the whole 9.6 KB buffer */
    memset(c->bits, 0, (size_t)c->h * (KUI_MAX_W / 8));
    kui_clip_reset(c);
}

void kui_clip(kui_canvas_t *c, int x, int y, int w, int h) {
    c->clip_x0 = x < 0 ? 0 : x;
    c->clip_y0 = y < 0 ? 0 : y;
    c->clip_x1 = x + w > c->w ? c->w : x + w;
    c->clip_y1 = y + h > c->h ? c->h : y + h;
}

void kui_clip_reset(kui_canvas_t *c) {
    c->clip_x0 = c->clip_y0 = 0;
    c->clip_x1 = c->w;
    c->clip_y1 = c->h;
}

static inline void put(kui_canvas_t *c, int x, int y, bool on) {
    if (x < c->clip_x0 || x >= c->clip_x1 || y < c->clip_y0 || y >= c->clip_y1) return;
    int i = y * KUI_MAX_W + x;
    uint8_t m = (uint8_t)(0x80 >> (i & 7));
    if (on) c->bits[i >> 3] |= m;
    else c->bits[i >> 3] &= (uint8_t)~m;
}

void kui_clear(kui_canvas_t *c, bool on) {
    /* The reference clears only the clip (two-pass transitions rely on it): the whole canvas when no clip is set. */
    for (int y = c->clip_y0; y < c->clip_y1; y++)
        for (int x = c->clip_x0; x < c->clip_x1; x++) put(c, x, y, on);
}

void kui_pixel(kui_canvas_t *c, int x, int y, bool on) { put(c, x, y, on); }

bool kui_get(const kui_canvas_t *c, int x, int y) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return false;
    int i = y * KUI_MAX_W + x;
    return (c->bits[i >> 3] >> (7 - (i & 7))) & 1;
}

/* Clamp a rect to the clip. Returns false when nothing is left. */
static bool clamp(const kui_canvas_t *c, int *x, int *y, int *w, int *h) {
    int x1 = *x + *w, y1 = *y + *h;
    if (*x < c->clip_x0) *x = c->clip_x0;
    if (*y < c->clip_y0) *y = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    *w = x1 - *x;
    *h = y1 - *y;
    return *w > 0 && *h > 0;
}

void kui_fill(kui_canvas_t *c, int x, int y, int w, int h, bool on) {
    if (!clamp(c, &x, &y, &w, &h)) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) put(c, xx, yy, on);
}

void kui_invert(kui_canvas_t *c, int x, int y, int w, int h) {
    if (!clamp(c, &x, &y, &w, &h)) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) put(c, xx, yy, !kui_get(c, xx, yy));
}

void kui_frame(kui_canvas_t *c, int x, int y, int w, int h, bool on) {
    kui_fill(c, x, y, w, 1, on);
    kui_fill(c, x, y + h - 1, w, 1, on);
    kui_fill(c, x, y, 1, h, on);
    kui_fill(c, x + w - 1, y, 1, h, on);
}

void kui_line(kui_canvas_t *c, int x0, int y0, int x1, int y1, bool on) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int x = x0, y = y0;
    for (;;) {
        put(c, x, y, on);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

void kui_fill_round(kui_canvas_t *c, int x, int y, int w, int h, int r, bool on) {
    if (r == 0 || w < 2 * r + 1 || h < 2 * r + 1) { kui_fill(c, x, y, w, h, on); return; }
    /* Full-width middle band, then 45 degree chamfers: each corner row narrows by one px per side. */
    kui_fill(c, x, y + r, w, h - 2 * r, on);
    for (int i = 0; i < r; i++) {
        int inset = r - i, bw = w - 2 * inset;
        kui_fill(c, x + inset, y + i, bw, 1, on);
        kui_fill(c, x + inset, y + h - 1 - i, bw, 1, on);
    }
}

void kui_round(kui_canvas_t *c, int x, int y, int w, int h, int r, bool on) {
    if (r == 0 || w < 2 * r + 1 || h < 2 * r + 1) { kui_frame(c, x, y, w, h, on); return; }
    kui_fill(c, x + r, y, w - 2 * r, 1, on);
    kui_fill(c, x + r, y + h - 1, w - 2 * r, 1, on);
    kui_fill(c, x, y + r, 1, h - 2 * r, on);
    kui_fill(c, x + w - 1, y + r, 1, h - 2 * r, on);
    kui_line(c, x + r, y, x, y + r, on);
    kui_line(c, x + w - 1 - r, y, x + w - 1, y + r, on);
    kui_line(c, x + r, y + h - 1, x, y + h - 1 - r, on);
    kui_line(c, x + w - 1 - r, y + h - 1, x + w - 1, y + h - 1 - r, on);
}

void kui_bitmap(kui_canvas_t *c, int x, int y, int w, int h, const uint8_t *bits, bool on) {
    int bpr = (w + 7) / 8;
    for (int r = 0; r < h; r++)
        for (int col = 0; col < w; col++)
            if (bits[r * bpr + col / 8] & (0x80 >> (col & 7))) put(c, x + col, y + r, on);
}

void kui_present(pal_canvas_t *dst, const kui_canvas_t *c, pal_rect_t r) {
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > dst->w ? dst->w : r.x + r.w;
    int y1 = r.y + r.h > dst->h ? dst->h : r.y + r.h;
    if (x0 < dst->clip_x0) x0 = dst->clip_x0;
    if (y0 < dst->clip_y0) y0 = dst->clip_y0;
    if (x1 > dst->clip_x1) x1 = dst->clip_x1;
    if (y1 > dst->clip_y1) y1 = dst->clip_y1;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) {
        uint8_t *row = dst->px + y * dst->stride;
        int ly = kui_lg(c, y);
        for (int x = x0; x < x1; x++) {
            bool on = kui_get(c, kui_lg(c, x), ly);
            row[x] = on ? PAL_C_INK : PAL_C_PAPER;
        }
    }
    pal_mark(dst, x0, y0, x1 - x0, y1 - y0);
}
