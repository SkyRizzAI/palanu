/* Box drawing (U+2500-257F) and block elements (U+2580-259F), drawn by the terminal to the cell size instead of taken from
 * the font, as most terminals do: frames of modern terminal UIs (ink, Bubble Tea, htop, tmux) then join from cell to cell
 * at any font size, and the small fonts, which have only a few of these glyphs, still show them. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pal/font.h"
#include "pal/gfx.h"
#include "term_int.h"

/* Each box character as four arms, 2 bits each, left | right << 2 | up << 4 | down << 6: 0 none, 1 light, 2 heavy, 3 double.
 * The dashed ones (2504-250B, 254C-254F) are drawn solid. Arcs (256D-2570) are corners drawn round below. */
#define A(l, r, u, d) (uint8_t)((l) | (r) << 2 | (u) << 4 | (d) << 6)
static const uint8_t BOX[128] = {
    A(1, 1, 0, 0), A(2, 2, 0, 0), A(0, 0, 1, 1), A(0, 0, 2, 2), A(1, 1, 0, 0), A(2, 2, 0, 0), A(0, 0, 1, 1), A(0, 0, 2, 2), /* 2500 */
    A(1, 1, 0, 0), A(2, 2, 0, 0), A(0, 0, 1, 1), A(0, 0, 2, 2), A(0, 1, 0, 1), A(0, 2, 0, 1), A(0, 1, 0, 2), A(0, 2, 0, 2), /* 2508 */
    A(1, 0, 0, 1), A(2, 0, 0, 1), A(1, 0, 0, 2), A(2, 0, 0, 2), A(0, 1, 1, 0), A(0, 2, 1, 0), A(0, 1, 2, 0), A(0, 2, 2, 0), /* 2510 */
    A(1, 0, 1, 0), A(2, 0, 1, 0), A(1, 0, 2, 0), A(2, 0, 2, 0), A(0, 1, 1, 1), A(0, 2, 1, 1), A(0, 1, 2, 1), A(0, 1, 1, 2), /* 2518 */
    A(0, 1, 2, 2), A(0, 2, 2, 1), A(0, 2, 1, 2), A(0, 2, 2, 2), A(1, 0, 1, 1), A(2, 0, 1, 1), A(1, 0, 2, 1), A(1, 0, 1, 2), /* 2520 */
    A(1, 0, 2, 2), A(2, 0, 2, 1), A(2, 0, 1, 2), A(2, 0, 2, 2), A(1, 1, 0, 1), A(2, 1, 0, 1), A(1, 2, 0, 1), A(2, 2, 0, 1), /* 2528 */
    A(1, 1, 0, 2), A(2, 1, 0, 2), A(1, 2, 0, 2), A(2, 2, 0, 2), A(1, 1, 1, 0), A(2, 1, 1, 0), A(1, 2, 1, 0), A(2, 2, 1, 0), /* 2530 */
    A(1, 1, 2, 0), A(2, 1, 2, 0), A(1, 2, 2, 0), A(2, 2, 2, 0), A(1, 1, 1, 1), A(2, 1, 1, 1), A(1, 2, 1, 1), A(2, 2, 1, 1), /* 2538 */
    A(1, 1, 2, 1), A(1, 1, 1, 2), A(1, 1, 2, 2), A(2, 1, 2, 1), A(1, 2, 2, 1), A(2, 1, 1, 2), A(1, 2, 1, 2), A(2, 2, 2, 1), /* 2540 */
    A(2, 2, 1, 2), A(2, 1, 2, 2), A(1, 2, 2, 2), A(2, 2, 2, 2), A(1, 1, 0, 0), A(2, 2, 0, 0), A(0, 0, 1, 1), A(0, 0, 2, 2), /* 2548 */
    A(3, 3, 0, 0), A(0, 0, 3, 3), A(0, 3, 0, 1), A(0, 1, 0, 3), A(0, 3, 0, 3), A(3, 0, 0, 1), A(1, 0, 0, 3), A(3, 0, 0, 3), /* 2550 */
    A(0, 3, 1, 0), A(0, 1, 3, 0), A(0, 3, 3, 0), A(3, 0, 1, 0), A(1, 0, 3, 0), A(3, 0, 3, 0), A(0, 3, 1, 1), A(0, 1, 3, 3), /* 2558 */
    A(0, 3, 3, 3), A(3, 0, 1, 1), A(1, 0, 3, 3), A(3, 0, 3, 3), A(3, 3, 0, 1), A(1, 1, 0, 3), A(3, 3, 0, 3), A(3, 3, 1, 0), /* 2560 */
    A(1, 1, 3, 0), A(3, 3, 3, 0), A(3, 3, 1, 1), A(1, 1, 3, 3), A(3, 3, 3, 3), A(0, 1, 0, 1), A(1, 0, 0, 1), A(1, 0, 1, 0), /* 2568 */
    A(0, 1, 1, 0), 0, 0, 0, A(1, 0, 0, 0), A(0, 0, 1, 0), A(0, 1, 0, 0), A(0, 0, 0, 1),                                   /* 2570 */
    A(2, 0, 0, 0), A(0, 0, 2, 0), A(0, 2, 0, 0), A(0, 0, 0, 2), A(1, 2, 0, 0), A(0, 0, 1, 2), A(2, 1, 0, 0), A(0, 0, 2, 1), /* 2578 */
};
#undef A

/* A horizontal arm from x0 to x1 (inclusive) at the centre row, in weight w (1 light, 2 heavy, 3 double). */
static void harm(pal_canvas_t *c, int x0, int x1, int cy, int w, pal_color_t fg) {
    if (!w || x1 < x0) return;
    if (w == 1) pal_fill(c, x0, cy, x1 - x0 + 1, 1, fg);
    else if (w == 2) pal_fill(c, x0, cy - 1, x1 - x0 + 1, 2, fg);
    else {
        pal_fill(c, x0, cy - 1, x1 - x0 + 1, 1, fg);
        pal_fill(c, x0, cy + 1, x1 - x0 + 1, 1, fg);
    }
}

static void varm(pal_canvas_t *c, int y0, int y1, int cx, int w, pal_color_t fg) {
    if (!w || y1 < y0) return;
    if (w == 1) pal_fill(c, cx, y0, 1, y1 - y0 + 1, fg);
    else if (w == 2) pal_fill(c, cx - 1, y0, 2, y1 - y0 + 1, fg);
    else {
        pal_fill(c, cx - 1, y0, 1, y1 - y0 + 1, fg);
        pal_fill(c, cx + 1, y0, 1, y1 - y0 + 1, fg);
    }
}

static void box(pal_canvas_t *c, int x, int y, int w, int h, uint32_t cp, pal_color_t fg) {
    int cx = x + (w - 1) / 2, cy = y + (h - 1) / 2;
    if (cp >= 0x2571 && cp <= 0x2573) { /* diagonals */
        for (int i = 0; i < h; i++) {
            int dx = i * (w - 1) / (h > 1 ? h - 1 : 1);
            if (cp != 0x2572) pal_fill(c, x + w - 1 - dx, y + i, 1, 1, fg);
            if (cp != 0x2571) pal_fill(c, x + dx, y + i, 1, 1, fg);
        }
        return;
    }
    uint8_t a = BOX[cp - 0x2500];
    int l = a & 3, r = (a >> 2) & 3, u = (a >> 4) & 3, d = (a >> 6) & 3;
    bool arc = cp >= 0x256D && cp <= 0x2570; /* the round corners: the arms stop short of the centre */
    int k = arc ? 1 : 0;
    /* arms meet at the centre; a double arm reaches past the centre only as far as the crossing line needs */
    harm(c, x, cx - k, cy, l, fg);
    harm(c, cx + k, x + w - 1, cy, r, fg);
    varm(c, y, cy - k, cx, u, fg);
    varm(c, cy + k, y + h - 1, cx, d, fg);
    if (!arc && (l || r) && (u || d) && !(l == 3 || r == 3 || u == 3 || d == 3)) pal_fill(c, cx, cy, 1, 1, fg);
}

/* Shades: a regular dot pattern of 1 in 4, 2 in 4 or 3 in 4 pixels. */
static void shade(pal_canvas_t *c, int x, int y, int w, int h, int level, pal_color_t fg) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int p = (i & 1) + 2 * (j & 1); /* 0..3 in a 2x2 cell */
            bool on = level == 1 ? p == 0 : level == 2 ? (p == 0 || p == 3) : p != 3;
            if (on) pal_fill(c, x + i, y + j, 1, 1, fg);
        }
}

static void block(pal_canvas_t *c, int x, int y, int w, int h, uint32_t cp, pal_color_t fg) {
    if (cp == 0x2580) pal_fill(c, x, y, w, h / 2, fg);                             /* upper half */
    else if (cp >= 0x2581 && cp <= 0x2588) {                                      /* lower 1/8 .. full */
        int n = (int)(cp - 0x2580), hh = (h * n + 4) / 8;
        pal_fill(c, x, y + h - hh, w, hh, fg);
    } else if (cp >= 0x2589 && cp <= 0x258F) {                                    /* left 7/8 .. 1/8 */
        int n = 8 - (int)(cp - 0x2588), ww = (w * n + 4) / 8;
        pal_fill(c, x, y, ww > 0 ? ww : 1, h, fg);
    } else if (cp == 0x2590) pal_fill(c, x + w / 2, y, w - w / 2, h, fg);          /* right half */
    else if (cp >= 0x2591 && cp <= 0x2593) shade(c, x, y, w, h, (int)(cp - 0x2590), fg);
    else if (cp == 0x2594) pal_fill(c, x, y, w, (h + 7) / 8, fg);                  /* upper 1/8 */
    else if (cp == 0x2595) pal_fill(c, x + w - (w + 7) / 8, y, (w + 7) / 8, h, fg); /* right 1/8 */
    else {                                                                         /* quadrants 2596..259F */
        static const uint8_t Q[10] = {4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8};
        uint8_t q = Q[cp - 0x2596];
        int hw = w / 2, hh = h / 2;
        if (q & 1) pal_fill(c, x, y, hw, hh, fg);
        if (q & 2) pal_fill(c, x + hw, y, w - hw, hh, fg);
        if (q & 4) pal_fill(c, x, y + hh, hw, h - hh, fg);
        if (q & 8) pal_fill(c, x + hw, y + hh, w - hw, h - hh, fg);
    }
}

/* ---- symbols the small fonts lack: drawn only when the font has no glyph (the 8x16 font keeps its own) ---- */

/* A filled or outlined square, circle or diamond of side s centred in the cell. */
static void shape(pal_canvas_t *c, int x, int y, int w, int h, int s, int kind, bool fill, pal_color_t fg) {
    int ox = x + (w - s) / 2, oy = y + (h - s) / 2;
    for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++) {
            int di = 2 * i - (s - 1), dj = 2 * j - (s - 1); /* doubled offsets from the centre */
            bool in, edge;
            if (kind == 0) { in = true; edge = i == 0 || j == 0 || i == s - 1 || j == s - 1; }
            else if (kind == 1) { int r2 = di * di + dj * dj, R = s * s; in = r2 <= R; edge = in && r2 > (s - 2) * (s - 2); }
            else { int m = (di < 0 ? -di : di) + (dj < 0 ? -dj : dj); in = m <= s - 1; edge = in && m > s - 3; }
            if (fill ? in : edge) pal_fill(c, ox + i, oy + j, 1, 1, fg);
        }
}

/* A triangle pointing dir (0 up, 1 right, 2 down, 3 left), filled, in a box of side s. */
static void tri(pal_canvas_t *c, int x, int y, int w, int h, int s, int dir, pal_color_t fg) {
    int ox = x + (w - s) / 2, oy = y + (h - s) / 2;
    for (int k = 0; k < s; k++) {
        int half = k / 2; /* the row k from the tip is 2*half+1 wide */
        int len = 2 * half + 1, start = (s - len) / 2;
        if (dir == 0) pal_fill(c, ox + start, oy + k, len, 1, fg);
        else if (dir == 2) pal_fill(c, ox + start, oy + s - 1 - k, len, 1, fg);
        else if (dir == 1) pal_fill(c, ox + k, oy + start, 1, len, fg);
        else pal_fill(c, ox + s - 1 - k, oy + start, 1, len, fg);
    }
}

static void arrow(pal_canvas_t *c, int x, int y, int w, int h, int dir, pal_color_t fg) {
    int cx = x + (w - 1) / 2, cy = y + (h - 1) / 2, n = (w < h ? w : h) / 2;
    /* the shaft, then the two strokes of the head */
    if (dir == 1 || dir == 3) pal_fill(c, x + 1, cy, w - 2, 1, fg);
    else pal_fill(c, cx, y + (h - 2 * n) / 2, 1, 2 * n, fg);
    for (int k = 1; k <= n / 2 + 1; k++) {
        if (dir == 1) { pal_fill(c, x + w - 2 - k, cy - k, 1, 1, fg); pal_fill(c, x + w - 2 - k, cy + k, 1, 1, fg); }
        if (dir == 3) { pal_fill(c, x + 1 + k, cy - k, 1, 1, fg); pal_fill(c, x + 1 + k, cy + k, 1, 1, fg); }
        if (dir == 0) { int ty = y + (h - 2 * n) / 2; pal_fill(c, cx - k, ty + k, 1, 1, fg); pal_fill(c, cx + k, ty + k, 1, 1, fg); }
        if (dir == 2) { int by = y + (h - 2 * n) / 2 + 2 * n - 1; pal_fill(c, cx - k, by - k, 1, 1, fg); pal_fill(c, cx + k, by - k, 1, 1, fg); }
    }
}

static void check(pal_canvas_t *c, int x, int y, int w, int h, bool cross, pal_color_t fg) {
    int s = (w < h ? w : h) - 1, ox = x + (w - s) / 2, oy = y + (h - s) / 2;
    for (int i = 0; i < s; i++) {
        if (cross) {
            pal_fill(c, ox + i, oy + i, 1, 1, fg);
            pal_fill(c, ox + s - 1 - i, oy + i, 1, 1, fg);
        } else {
            int k = s / 3; /* the short stroke down to k, then up to the right */
            int yy = i <= k ? oy + s / 2 + i : oy + s / 2 + k - (i - k) * (s / 2 + k) / (s - 1 - k > 0 ? s - 1 - k : 1);
            pal_fill(c, ox + i, yy, 1, 1, fg);
        }
    }
}

static bool symbol(pal_canvas_t *c, int x, int y, int w, int h, uint32_t cp, pal_color_t fg) {
    int s = (w < h ? w : h) - 1; /* the shape's side: a pixel of air */
    int small = s / 2 + 1;
    switch (cp) {
    case 0x2022: shape(c, x, y, w, h, small, 1, true, fg); return true;                 /* • */
    case 0xFF65: case 0x2219: case 0x22C5: { /* ･ ∙ ⋅ (the empty part of a progress bar): a dot centred in the cell */
        int d = h >= 12 ? 2 : 1;
        pal_fill(c, x + (w - d) / 2, y + (h - d) / 2, d, d, fg);
        return true;
    }
    case 0x2026: for (int i = 0; i < 3; i++) pal_fill(c, x + i * w / 3 + w / 6, y + h - 3, 1, 1, fg); return true; /* … */
    case 0x2190: arrow(c, x, y, w, h, 3, fg); return true;
    case 0x2191: arrow(c, x, y, w, h, 0, fg); return true;
    case 0x2192: arrow(c, x, y, w, h, 1, fg); return true;
    case 0x2193: arrow(c, x, y, w, h, 2, fg); return true;
    case 0x25A0: shape(c, x, y, w, h, s - 1, 0, true, fg); return true;                 /* ■ */
    case 0x25A1: shape(c, x, y, w, h, s - 1, 0, false, fg); return true;                /* □ */
    case 0x25AA: shape(c, x, y, w, h, small, 0, true, fg); return true;                 /* ▪ */
    case 0x25AB: shape(c, x, y, w, h, small, 0, false, fg); return true;                /* ▫ */
    case 0x25B2: case 0x25B4: tri(c, x, y, w, h, s, 0, fg); return true;                 /* ▲ ▴ */
    case 0x25B6: case 0x25B8: tri(c, x, y, w, h, s, 1, fg); return true;                 /* ▶ ▸ */
    case 0x25BC: case 0x25BE: tri(c, x, y, w, h, s, 2, fg); return true;                 /* ▼ ▾ */
    case 0x25C0: case 0x25C2: tri(c, x, y, w, h, s, 3, fg); return true;                 /* ◀ ◂ */
    case 0x25C6: shape(c, x, y, w, h, s, 2, true, fg); return true;                     /* ◆ */
    case 0x25C7: shape(c, x, y, w, h, s, 2, false, fg); return true;                    /* ◇ */
    case 0x25CB: case 0x25EF: shape(c, x, y, w, h, s, 1, false, fg); return true;       /* ○ */
    case 0x25CF: shape(c, x, y, w, h, s, 1, true, fg); return true;                     /* ● */
    case 0x2713: case 0x2714: check(c, x, y, w, h, false, fg); return true;             /* ✓ ✔ */
    case 0x2717: case 0x2718: check(c, x, y, w, h, true, fg); return true;              /* ✗ ✘ */
    default: return false;
    }
}


/* ---- Braille (U+2800-28FF) and Powerline (U+E0A0-E0BF): always drawn to the cell, as modern terminals do, so spinners, graphs and
 * prompts look the same in every font size ---- */

/* Braille: a 2x4 grid of dots, bit 0..7 = dots 1,2,3,7 down the left column and 4,5,6,8 down the right. */
static void braille(pal_canvas_t *c, int x, int y, int w, int h, uint32_t cp, pal_color_t fg) {
    static const uint8_t POS[8][2] = {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}};
    int d = h >= 12 && w >= 8 ? 2 : 1; /* dot side */
    for (int b = 0; b < 8; b++) {
        if (!(cp & (1u << b))) continue;
        int cx = x + (POS[b][0] * 2 + 1) * w / 4 - d / 2; /* centres at 1/4 and 3/4 of the width */
        int cy = y + (POS[b][1] * 2 + 1) * h / 8 - d / 2; /* and 1/8, 3/8, 5/8, 7/8 of the height */
        if (d == 1 && w < 6) cx = x + POS[b][0] * (w - 1);
        pal_fill(c, cx, cy, d, d, fg);
    }
}

static float fabsf_(float v) { return v < 0 ? -v : v; }

/* One Powerline separator as a per-pixel rule over the cell: fx, fy in 0..1 at the pixel's centre. */
static bool powerline(pal_canvas_t *c, int x, int y, int w, int h, uint32_t cp, pal_color_t fg) {
    if (cp < 0xE0B0 || cp > 0xE0BF) return false;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            float fx = (i + 0.5f) / w, fy = (j + 0.5f) / h, px = i + 0.5f;
            float tri = 1.0f - fabsf_(2 * fy - 1); /* the triangle's edge: 0 at the corners, 1 in the middle */
            bool in = false;
            switch (cp) {
            case 0xE0B0: in = fx <= tri; break;                                     /*  solid, points right */
            case 0xE0B1: in = fabsf_(px - w * tri) < 0.8f && px <= w * tri + 0.8f; break; /*  chevron, right */
            case 0xE0B2: in = 1.0f - fx <= tri; break;                              /*  solid, points left */
            case 0xE0B3: in = fabsf_((float)w - px - w * tri) < 0.8f; break;        /*  chevron, left */
            case 0xE0B4: in = fx * fx + (2 * fy - 1) * (2 * fy - 1) <= 1.0f; break; /*  half circle, right */
            case 0xE0B5: { float r = fx * fx + (2 * fy - 1) * (2 * fy - 1); in = r <= 1.0f && r > 0.62f; break; }
            case 0xE0B6: { float u = 1.0f - fx; in = u * u + (2 * fy - 1) * (2 * fy - 1) <= 1.0f; break; }
            case 0xE0B7: { float u = 1.0f - fx, r = u * u + (2 * fy - 1) * (2 * fy - 1); in = r <= 1.0f && r > 0.62f; break; }
            case 0xE0B8: in = fy >= fx; break;                                      /* lower left triangle */
            case 0xE0B9: in = fabsf_(px - fy * w) < 0.8f; break;                    /* "\" */
            case 0xE0BA: in = fy >= 1.0f - fx; break;                               /* lower right triangle */
            case 0xE0BB: in = fabsf_(px - (1.0f - fy) * w) < 0.8f; break;           /* "/" */
            case 0xE0BC: in = fy <= 1.0f - fx; break;                               /* upper left triangle */
            case 0xE0BD: in = fabsf_(px - (1.0f - fy) * w) < 0.8f; break;           /* "/" */
            case 0xE0BE: in = fy <= fx; break;                                      /* upper right triangle */
            default: in = fabsf_(px - fy * w) < 0.8f; break;                        /* 0xE0BF "\" */
            }
            if (in) pal_fill(c, x + i, y + j, 1, 1, fg);
        }
    return true;
}

/* The four Powerline symbols that are not separators: branch, line number, lock, column. Simple marks that read at 8x16. */
static bool powerline_mark(pal_canvas_t *c, int x, int y, int w, int h, uint32_t cp, pal_color_t fg) {
    int sx = x + w / 3, dot = h >= 12 ? 2 : 1;
    switch (cp) {
    case 0xE0A0: /*  branch: a stem with a dot at each end and a branch curving off the top */
        pal_fill(c, sx, y + 2, 1, h - 4, fg);
        pal_fill(c, sx - dot / 2, y + 1, dot, dot, fg);
        pal_fill(c, sx - dot / 2, y + h - 2 - dot, dot, dot, fg);
        pal_fill(c, x + w - 2 - dot / 2, y + 1, dot, dot, fg);
        for (int k = 0; k < h / 3; k++) pal_fill(c, x + w - 2 - k * (w - 2 - w / 3) / (h / 3), y + 2 + k, 1, 1, fg);
        return true;
    case 0xE0A1: /*  line number: two rules and a short one */
        pal_fill(c, x + 1, y + h / 3, w - 2, 1, fg);
        pal_fill(c, x + 1, y + h / 2, w - 2, 1, fg);
        pal_fill(c, x + 1, y + h * 2 / 3, w / 2, 1, fg);
        return true;
    case 0xE0A2: /*  lock: a body and a shackle */
        pal_fill(c, x + 1, y + h / 2, w - 2, h / 3 + 1, fg);
        pal_fill(c, x + w / 4 + 1, y + h / 4, 1, h / 4, fg);
        pal_fill(c, x + w - w / 4 - 2, y + h / 4, 1, h / 4, fg);
        pal_fill(c, x + w / 4 + 1, y + h / 4, w / 2 - 2, 1, fg);
        return true;
    case 0xE0A3: /*  column: a thin vertical bar with a cap */
        pal_fill(c, sx, y + 1, 1, h - 2, fg);
        pal_fill(c, sx - 1, y + 1, 3, 1, fg);
        return true;
    default: return false;
    }
}


/* ---- the fallback font (font_fallback.c): Unifont and Nerd Font glyphs scaled to the cell, then the empty box ---- */

/* The outline of the cell, the usual "tofu": the line keeps its layout and the reader sees that a character is missing. */
static void tofu(pal_canvas_t *c, int x, int y, int w, int h, pal_color_t fg) {
    int bx = x + 1, by = y + 1, bw = w - 2, bh = h - 2;
    if (bw < 2 || bh < 2) return;
    pal_fill(c, bx, by, bw, 1, fg);
    pal_fill(c, bx, by + bh - 1, bw, 1, fg);
    pal_fill(c, bx, by, 1, bh, fg);
    pal_fill(c, bx + bw - 1, by, 1, bh, fg);
}

/* A 16-row bitmap of cells*8 columns scaled to w x h pixels: a target pixel is set when a third of its source area is, so thin
 * strokes survive the 5x8 and 6x12 fonts. At 8x16 (or 16x16) it is the identity. */
static bool blit_scaled(pal_canvas_t *c, int x, int y, int w, int h, const uint8_t *src, int cells, pal_color_t fg, pal_color_t bg) {
    uint8_t out[32 * 2];
    if (w < 1 || h < 1 || w > 16 || h > 32) return false;
    const int sw = cells * 8, bpr = (w + 7) / 8;
    memset(out, 0, (size_t)bpr * (size_t)h);
    for (int j = 0; j < h; j++) {
        int sy0 = j * 16 / h, sy1 = (j + 1) * 16 / h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int i = 0; i < w; i++) {
            int sx0 = i * sw / w, sx1 = (i + 1) * sw / w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int on = 0, total = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++, total++)
                    if (src[yy * cells + xx / 8] & (0x80 >> (xx % 8))) on++;
            if (on * 3 >= total) out[j * bpr + i / 8] |= (uint8_t)(0x80 >> (i % 8));
        }
    }
    pal_blit_mono(c, x, y, w, h, out, bpr, fg, bg);
    return true;
}

/* Draws cp from the fallback font into ncols cells of width cw: the glyph's own width (one or two cells) is scaled to the cell
 * size and centred in the columns. False when the font has no glyph. */
static bool fallback_glyph(pal_canvas_t *c, int x, int y, int cw, int ncols, int h, uint32_t cp, pal_color_t fg, pal_color_t bg) {
    int cells = 0;
    const uint8_t *g = pal_fallback_glyph(cp, &cells);
    if (!g) return false;
    int w = cells < ncols ? cells * cw : cw * ncols; /* a one-cell glyph in two columns keeps its width */
    return blit_scaled(c, x + (cw * ncols - w) / 2, y, w, h, g, cells, fg, bg);
}

bool term_special_glyph(pal_canvas_t *c, const pal_font_t *f, int x, int y, int w, int h, uint32_t cp, pal_color_t fg,
                        pal_color_t bg) {
    if (cp >= 0x2500 && cp <= 0x259F) {
        pal_fill(c, x, y, w, h, bg);
        if (cp < 0x2580) box(c, x, y, w, h, cp, fg);
        else block(c, x, y, w, h, cp, fg);
        return true;
    }
    if (cp >= 0x2800 && cp <= 0x28FF) {
        pal_fill(c, x, y, w, h, bg);
        braille(c, x, y, w, h, cp, fg);
        return true;
    }
    if (cp >= 0xE0A0 && cp <= 0xE0BF) {
        pal_fill(c, x, y, w, h, bg);
        if (powerline(c, x, y, w, h, cp, fg) || powerline_mark(c, x, y, w, h, cp, fg)) return true;
        return true; /* the unassigned E0A4-E0AF and the like: blank, not a '?' */
    }
    if (cp < 0x80 || pal_font_has(f, cp)) return false;
    pal_fill(c, x, y, w, h, bg);
    if (cp >= 0x2000 && symbol(c, x, y, w, h, cp, fg)) return true;
    if (fallback_glyph(c, x, y, w, 1, h, cp, fg, bg)) return true;
    tofu(c, x, y, w, h, fg); /* neither font has it: an empty box, not a '?' that reads as a real question mark */
    return true;
}

void term_wide_glyph(pal_canvas_t *c, const pal_font_t *f, int x, int y, int w, int h, uint32_t cp, pal_color_t fg, pal_color_t bg) {
    pal_fill(c, x, y, w, h, bg);
    if (pal_font_has(f, cp)) {
        pal_glyph(c, f, x + (w - f->w) / 2, y, cp, fg, bg);
        return;
    }
    if (fallback_glyph(c, x, y, w / 2, 2, h, cp, fg, bg)) return;
    tofu(c, x, y, w, h, fg);
}
