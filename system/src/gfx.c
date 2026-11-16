#include <stdlib.h>
#include <string.h>

#include "pal/gfx.h"

void pal_canvas_init(pal_canvas_t *c, void *pixels, int w, int h) {
    c->px = (uint8_t *)pixels;
    c->w = w;
    c->h = h;
    c->stride = w;
    pal_clip_reset(c);
    c->dirty_x0 = c->dirty_y0 = 0;
    c->dirty_x1 = c->dirty_y1 = 0;
}

void pal_clip_set(pal_canvas_t *c, int x, int y, int w, int h) {
    c->clip_x0 = x < 0 ? 0 : x;
    c->clip_y0 = y < 0 ? 0 : y;
    c->clip_x1 = x + w > c->w ? c->w : x + w;
    c->clip_y1 = y + h > c->h ? c->h : y + h;
}

void pal_clip_reset(pal_canvas_t *c) {
    c->clip_x0 = c->clip_y0 = 0;
    c->clip_x1 = c->w;
    c->clip_y1 = c->h;
}

void pal_mark(pal_canvas_t *c, int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x < c->clip_x0) x = c->clip_x0;
    if (y < c->clip_y0) y = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    if (x1 <= x || y1 <= y) return;
    if (c->dirty_x1 <= c->dirty_x0) {
        c->dirty_x0 = x, c->dirty_y0 = y, c->dirty_x1 = x1, c->dirty_y1 = y1;
        return;
    }
    if (x < c->dirty_x0) c->dirty_x0 = x;
    if (y < c->dirty_y0) c->dirty_y0 = y;
    if (x1 > c->dirty_x1) c->dirty_x1 = x1;
    if (y1 > c->dirty_y1) c->dirty_y1 = y1;
}

void pal_mark_all(pal_canvas_t *c) {
    c->dirty_x0 = c->dirty_y0 = 0;
    c->dirty_x1 = c->w;
    c->dirty_y1 = c->h;
}

bool pal_take_dirty(pal_canvas_t *c, pal_rect_t *r) {
    if (c->dirty_x1 <= c->dirty_x0 || c->dirty_y1 <= c->dirty_y0) return false;
    r->x = c->dirty_x0;
    r->y = c->dirty_y0;
    r->w = c->dirty_x1 - c->dirty_x0;
    r->h = c->dirty_y1 - c->dirty_y0;
    c->dirty_x0 = c->dirty_y0 = c->dirty_x1 = c->dirty_y1 = 0;
    return true;
}

void pal_clear(pal_canvas_t *c, pal_color_t color) {
    pal_clip_reset(c);
    pal_fill(c, 0, 0, c->w, c->h, color);
}

static inline void put(pal_canvas_t *c, int x, int y, uint8_t v) {
    if (x >= c->clip_x0 && x < c->clip_x1 && y >= c->clip_y0 && y < c->clip_y1) c->px[y * c->stride + x] = v;
}

void pal_pixel(pal_canvas_t *c, int x, int y, pal_color_t color) {
    if (color < 0) return;
    put(c, x, y, (uint8_t)color);
    pal_mark(c, x, y, 1, 1);
}

pal_color_t pal_get(const pal_canvas_t *c, int x, int y) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
    return c->px[y * c->stride + x];
}

void pal_fill(pal_canvas_t *c, int x, int y, int w, int h, pal_color_t color) {
    if (color < 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < c->clip_x0) x = c->clip_x0;
    if (y < c->clip_y0) y = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    if (x1 <= x || y1 <= y) return;
    for (int yy = y; yy < y1; yy++) memset(c->px + yy * c->stride + x, color, (size_t)(x1 - x));
    pal_mark(c, x, y, x1 - x, y1 - y);
}

void pal_rect(pal_canvas_t *c, int x, int y, int w, int h, pal_color_t color) {
    if (w <= 0 || h <= 0) return;
    pal_fill(c, x, y, w, 1, color);
    pal_fill(c, x, y + h - 1, w, 1, color);
    pal_fill(c, x, y, 1, h, color);
    pal_fill(c, x + w - 1, y, 1, h, color);
}

void pal_line(pal_canvas_t *c, int x0, int y0, int x1, int y1, pal_color_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pal_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void pal_circle(pal_canvas_t *c, int cx, int cy, int r, pal_color_t color) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        pal_pixel(c, cx + x, cy + y, color); pal_pixel(c, cx - x, cy + y, color);
        pal_pixel(c, cx + x, cy - y, color); pal_pixel(c, cx - x, cy - y, color);
        pal_pixel(c, cx + y, cy + x, color); pal_pixel(c, cx - y, cy + x, color);
        pal_pixel(c, cx + y, cy - x, color); pal_pixel(c, cx - y, cy - x, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void pal_fill_circle(pal_canvas_t *c, int cx, int cy, int r, pal_color_t color) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        pal_fill(c, cx - x, cy + y, 2 * x + 1, 1, color);
        pal_fill(c, cx - x, cy - y, 2 * x + 1, 1, color);
        pal_fill(c, cx - y, cy + x, 2 * y + 1, 1, color);
        pal_fill(c, cx - y, cy - x, 2 * y + 1, 1, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* How many pixels row i (counted from the nearest top or bottom edge) is cut by a corner of radius r. */
static int inset(int r, int i) {
    static const uint8_t T[5][4] = {{0}, {1}, {2, 1}, {3, 1, 1}, {4, 2, 1, 1}};
    if (r <= 0 || i >= r) return 0;
    if (r <= 4) return T[r][i];
    int d = r - i, v = 0;
    while ((v + 1) * (v + 1) <= r * r - d * d) v++;
    return r - v;
}

static int row_inset(int r, int h, int i) {
    if (i < 0 || i >= h) return 1 << 14;
    return inset(r, i < h - 1 - i ? i : h - 1 - i);
}

void pal_fill_round(pal_canvas_t *c, int x, int y, int w, int h, int r, pal_color_t color) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int i = 0; i < h; i++) {
        int in = row_inset(r, h, i);
        pal_fill(c, x + in, y + i, w - 2 * in, 1, color);
    }
}

void pal_round(pal_canvas_t *c, int x, int y, int w, int h, int r, pal_color_t color) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int i = 0; i < h; i++) {
        int in = row_inset(r, h, i);
        int lim = row_inset(r, h, i - 1);
        int dn = row_inset(r, h, i + 1);
        if (dn > lim) lim = dn;
        if (lim > w / 2) lim = w - in;
        int span = lim > in ? lim - in : 1;
        if (span * 2 >= w - 2 * in) {
            pal_fill(c, x + in, y + i, w - 2 * in, 1, color);
        } else {
            pal_fill(c, x + in, y + i, span, 1, color);
            pal_fill(c, x + w - in - span, y + i, span, 1, color);
        }
    }
}

void pal_blit_mono(pal_canvas_t *c, int x, int y, int w, int h, const uint8_t *bits, int bpr, pal_color_t fg,
                   pal_color_t bg) {
    if (bg >= 0) pal_fill(c, x, y, w, h, bg);
    if (fg < 0) return;
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < c->clip_y0 || yy >= c->clip_y1) continue;
        const uint8_t *row = bits + r * bpr;
        for (int col = 0; col < w; col++) {
            if (!(row[col >> 3] & (0x80 >> (col & 7)))) continue;
            put(c, x + col, yy, (uint8_t)fg);
        }
    }
    pal_mark(c, x, y, w, h);
}

void pal_blit(pal_canvas_t *c, int x, int y, int w, int h, const uint8_t *src, int src_stride) {
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < c->clip_y0 || yy >= c->clip_y1) continue;
        int x0 = x < c->clip_x0 ? c->clip_x0 : x;
        int x1 = x + w > c->clip_x1 ? c->clip_x1 : x + w;
        if (x1 <= x0) continue;
        memcpy(c->px + yy * c->stride + x0, src + r * src_stride + (x0 - x), (size_t)(x1 - x0));
    }
    pal_mark(c, x, y, w, h);
}

void pal_glyph(pal_canvas_t *c, const pal_font_t *f, int x, int y, uint32_t cp, pal_color_t fg, pal_color_t bg) {
    pal_blit_mono(c, x, y, f->w, f->h, pal_font_glyph(f, cp), pal_font_bpr(f), fg, bg);
}

int pal_text(pal_canvas_t *c, const pal_font_t *f, int x, int y, const char *s, pal_color_t fg, pal_color_t bg) {
    for (uint32_t cp; (cp = pal_utf8_next(&s)) != 0;) {
        pal_glyph(c, f, x, y, cp, fg, bg);
        x += f->w;
    }
    return x;
}

int pal_text_w(const pal_font_t *f, const char *s) {
    int n = 0;
    while (pal_utf8_next(&s)) n++;
    return n * f->w;
}
