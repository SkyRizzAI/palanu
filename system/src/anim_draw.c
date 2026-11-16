/* Animation playback and drawing (docs/ANIM.md, "Playback" and "Drawing"). The player is a pure state machine over the caller's clock:
 * a passive loop, and the active sequence once when triggered. Frames are drawn with nearest-neighbour sampling, and every draw
 * marks only the rectangle it covered, so the display sends the animation and nothing else. */
#include <string.h>

#include "anim_int.h"
#include "pal/theme.h"

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* ---- palette slots ---- */

pal_color_t pal_anim_slot(uint16_t v, uint8_t mono_override) {
    if (mono_override == 1) return PAL_C_INK;
    if (mono_override == 2) return PAL_C_PAPER;
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    int hi = r > g ? r : g;
    if (b > hi) hi = b;
    int lo = r < g ? r : g;
    if (b < lo) lo = b;
    if (hi - lo >= 96) return PAL_C_ACCENT;
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    if (lum < 96) return PAL_C_INK;
    if (lum < 176) return PAL_C_DIM;
    if (lum < 232) return PAL_C_PANEL;
    return PAL_C_PAPER;
}

/* ---- playback ---- */

void pal_anim_play_start(pal_anim_player_t *p, const pal_anim_t *a, uint32_t now_ms) {
    memset(p, 0, sizeof(*p));
    p->a = a;
    p->last_ms = now_ms;
}

void pal_anim_play_trigger(pal_anim_player_t *p) {
    if (!p->a || p->a->p.info.active == 0) return;
    p->active = true;
    p->playhead = p->a->p.info.passive;
}

void pal_anim_play_seek(pal_anim_player_t *p, int ph) {
    if (!p->a) return;
    int n = p->a->p.info.positions;
    if (ph < 0) ph = 0;
    if (ph >= n) ph = n - 1;
    p->active = false;
    p->playhead = ph;
}

int pal_anim_play_bitmap(const pal_anim_player_t *p) { return p->a ? anim_bitmap_at(p->a, p->playhead) : 0; }

/* One position forward: the active sequence runs to its end and hands back to the passive loop; the passive loop wraps. */
static void advance(pal_anim_player_t *p) {
    const pal_anim_info_t *i = &p->a->p.info;
    if (p->active) {
        int end = i->passive + i->active;
        if (end > i->positions) end = i->positions;
        if (p->playhead + 1 < end) {
            p->playhead++;
        } else {
            p->active = false;
            p->playhead = 0;
        }
        return;
    }
    int pend = i->passive > 0 ? i->passive : i->positions;
    if (pend > i->positions) pend = i->positions;
    p->playhead = p->playhead + 1 < pend ? p->playhead + 1 : 0;
}

bool pal_anim_play_tick(pal_anim_player_t *p, uint32_t now_ms) {
    if (!p->a) return false;
    uint32_t fps = p->a->p.info.fps;
    uint32_t ms = fps ? 1000u / fps : 1000u;
    if (ms == 0) ms = 1;
    if (!p->started) {
        p->started = true;
        p->last_ms = now_ms;
        return false;
    }
    uint32_t elapsed = now_ms - p->last_ms; /* unsigned: wraps correctly across the 32-bit clock */
    if (elapsed < ms) return false;
    uint32_t adv = elapsed / ms;
    if (adv > (uint32_t)p->a->p.info.positions) adv = (uint32_t)p->a->p.info.positions;
    p->last_ms = now_ms - elapsed % ms;
    int before = pal_anim_play_bitmap(p);
    for (uint32_t k = 0; k < adv; k++) advance(p);
    return pal_anim_play_bitmap(p) != before;
}

/* ---- layout ---- */

pal_rect_t pal_anim_fit_rect(int w, int h, pal_rect_t area, pal_anim_fit_t fit, pal_anchor_t anchor) {
    int dw = w, dh = h;
    if (w <= 0 || h <= 0 || area.w <= 0 || area.h <= 0) return (pal_rect_t){area.x, area.y, 0, 0};
    switch (fit) {
    case PAL_ANIM_FIT_STRETCH:
        dw = area.w;
        dh = area.h;
        break;
    case PAL_ANIM_FIT_FIT:
        if (area.w * h <= area.h * w) {
            dw = area.w;
            dh = h * area.w / w;
        } else {
            dh = area.h;
            dw = w * area.h / h;
        }
        break;
    case PAL_ANIM_FIT_CROP:
        if (area.w * h >= area.h * w) {
            dw = area.w;
            dh = h * area.w / w;
        } else {
            dh = area.h;
            dw = w * area.h / h;
        }
        break;
    case PAL_ANIM_FIT_CENTER:
    default:
        break;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int col = (int)anchor % 3, row = (int)anchor / 3; /* 0 near edge, 1 middle, 2 far edge */
    pal_rect_t r;
    r.x = area.x + floordiv((area.w - dw) * col, 2);
    r.y = area.y + floordiv((area.h - dh) * row, 2);
    r.w = dw;
    r.h = dh;
    return r;
}

/* ---- drawing ---- */

/* Draws bitmap bm into r (the whole animation mapped onto r), clipped to area and to the canvas clip. */
static void draw_into(pal_canvas_t *c, const pal_anim_t *a, int bm, pal_rect_t r, pal_rect_t area, pal_color_t on,
                      pal_color_t off) {
    const pal_anim_info_t *inf = &a->p.info;
    if (bm < 0 || bm >= inf->frames || r.w <= 0 || r.h <= 0) return;
    int x0 = r.x > area.x ? r.x : area.x, y0 = r.y > area.y ? r.y : area.y;
    int x1 = r.x + r.w < area.x + area.w ? r.x + r.w : area.x + area.w;
    int y1 = r.y + r.h < area.y + area.h ? r.y + r.h : area.y + area.h;
    if (x0 < c->clip_x0) x0 = c->clip_x0;
    if (y0 < c->clip_y0) y0 = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    if (x1 <= x0 || y1 <= y0) return;
    int sw = inf->width, sh = inf->height;
    const uint8_t *frame = a->data + a->p.frame0 + (size_t)bm * a->p.bpf;
    /* colours of the values (a nibble is 0 to 15): 0 is transparent (indexed) or off, 1 is on (mono) or the first palette
     * entry; a value past the palette is transparent too */
    pal_color_t map[16];
    map[0] = off;
    for (int v = 1; v < 16; v++) map[v] = -1;
    if (inf->indexed) {
        for (int v = 1; v <= a->p.npal; v++) map[v] = pal_anim_slot(a->p.pal_rgb[v - 1], a->p.pal_mo[v - 1]);
    } else {
        map[1] = on;
    }
    uint16_t sx_of[PAL_ANIM_MAX_W];
    for (int x = x0; x < x1; x++) {
        int sx = (int)((int64_t)(x - r.x) * sw / r.w);
        sx_of[x - x0] = (uint16_t)(sx < 0 ? 0 : sx >= sw ? sw - 1 : sx);
    }
    int bpr = inf->indexed ? (sw + 1) / 2 : 0;
    for (int y = y0; y < y1; y++) {
        int sy = (int)((int64_t)(y - r.y) * sh / r.h);
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;
        uint8_t *row = c->px + y * c->stride;
        for (int x = x0; x < x1; x++) {
            int sx = sx_of[x - x0];
            pal_color_t col;
            if (inf->indexed) {
                uint8_t b = frame[(size_t)sy * bpr + (sx >> 1)];
                col = map[(sx & 1) ? (b & 15) : (b >> 4)];
            } else {
                int idx = sy * sw + sx;
                col = (frame[idx >> 3] & (0x80 >> (idx & 7))) ? map[1] : map[0];
            }
            if (col >= 0) row[x] = (uint8_t)col;
        }
    }
    pal_mark(c, x0, y0, x1 - x0, y1 - y0);
}

void pal_anim_draw(pal_canvas_t *c, const pal_anim_t *a, int bm, int x, int y, int scale, pal_color_t on, pal_color_t off) {
    if (!c || !a) return;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    const pal_anim_info_t *inf = &a->p.info;
    pal_rect_t r = {x, y, inf->width * scale, inf->height * scale};
    pal_rect_t area = {0, 0, c->w, c->h};
    draw_into(c, a, bm, r, area, on, off);
}

void pal_anim_draw_fit(pal_canvas_t *c, const pal_anim_t *a, int bm, pal_rect_t area, pal_anim_fit_t fit, pal_anchor_t anchor,
                       pal_color_t on, pal_color_t off) {
    if (!c || !a) return;
    const pal_anim_info_t *inf = &a->p.info;
    pal_rect_t r = pal_anim_fit_rect(inf->width, inf->height, area, fit, anchor);
    if (off >= 0) {
        int x0 = area.x < c->clip_x0 ? c->clip_x0 : area.x;
        int y0 = area.y < c->clip_y0 ? c->clip_y0 : area.y;
        int x1 = area.x + area.w > c->clip_x1 ? c->clip_x1 : area.x + area.w;
        int y1 = area.y + area.h > c->clip_y1 ? c->clip_y1 : area.y + area.h;
        if (x1 > x0 && y1 > y0) {
            for (int y = y0; y < y1; y++) memset(c->px + y * c->stride + x0, off, (size_t)(x1 - x0));
            pal_mark(c, x0, y0, x1 - x0, y1 - y0);
        }
    }
    draw_into(c, a, bm, r, area, on, off);
}

/* ---- mono view for the desktop (kui) ---- */

const uint8_t *pal_anim_mono_bits(const pal_anim_t *a, int bm, uint8_t *buf, size_t cap) {
    const pal_anim_info_t *inf = &a->p.info;
    if (bm < 0 || bm >= inf->frames) bm = 0;
    const uint8_t *frame = a->data + a->p.frame0 + (size_t)bm * a->p.bpf;
    if (!inf->indexed) return frame; /* Mono1: the bits as stored, MSB first, row by row */
    size_t need = ((size_t)inf->width * inf->height + 7) / 8;
    if (!buf || cap < need) return NULL;
    /* Indexed4 (the reference baked fallback): a nibble is ink when its palette slot is ink. Nibble 0 is paper. */
    pal_color_t ink[16];
    for (int v = 0; v < 16; v++) ink[v] = PAL_C_PAPER;
    for (int v = 1; v <= a->p.npal; v++) ink[v] = pal_anim_slot(a->p.pal_rgb[v - 1], a->p.pal_mo[v - 1]);
    memset(buf, 0, need);
    int bpr = (inf->width + 1) / 2;
    for (int y = 0; y < inf->height; y++)
        for (int x = 0; x < inf->width; x++) {
            uint8_t b = frame[(size_t)y * bpr + (x >> 1)];
            uint8_t v = (x & 1) ? (b & 15) : (b >> 4);
            if (ink[v] != PAL_C_INK) continue;
            int idx = y * inf->width + x;
            buf[idx >> 3] |= (uint8_t)(0x80 >> (idx & 7));
        }
    return buf;
}
