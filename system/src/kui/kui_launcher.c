/* kui launcher skins: The four launcher skins on the logical 1-bit canvas. The geometry, the text roles
 * (Body and Caption REG8, Subhead BOLD8) and the default theme's spacing (xs 2, sm 4, lg 12) are the reference's; the arithmetic keeps
 * The reference integer steps so every logical pixel lands where the reference does. Icons are scaled nearest-neighbour as blitScaledMask
 * does, and paint only their set bits. */
#include <stdio.h>

#include "pal/kui.h"

#define SPACE_XS 2 /* The reference space.xs, default theme */
#define SPACE_SM 4 /* space.sm */
#define SPACE_LG 12 /* space.lg */

static bool src_bit(const uint8_t *bits, int sw, int sx, int sy) {
    int bpr = (sw + 7) / 8;
    return (bits[sy * bpr + (sx >> 3)] >> (7 - (sx & 7))) & 1;
}

void kui_bitmap_scaled(kui_canvas_t *c, const uint8_t *bits, int sw, int sh, int x, int y, int w, int h, bool on) {
    if (!bits || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    for (int dy = 0; dy < h; dy++) {
        int sy = dy * sh / h;
        if (sy >= sh) sy = sh - 1;
        for (int dx = 0; dx < w; dx++) {
            int sx = dx * sw / w;
            if (sx >= sw) sx = sw - 1;
            if (src_bit(bits, sw, sx, sy)) kui_pixel(c, x + dx, y + dy, on);
        }
    }
}

void kui_hscrollbar(kui_canvas_t *c, int x, int y, int size, int offset, int viewport, int content) {
    const int bar = 3, min_thumb = 4;
    if (size <= 0) return;
    for (int i = 0; i < size; i += 2) kui_fill(c, x + i, y + 1, 1, 1, true);
    if (content <= viewport || viewport <= 0) {
        kui_fill(c, x, y, size, bar, true);
        return;
    }
    int thumb = size * viewport / content;
    if (thumb < min_thumb) thumb = min_thumb;
    if (thumb > size) thumb = size;
    int max_scroll = content - viewport;
    int off = offset > max_scroll ? max_scroll : offset;
    int travel = size - thumb;
    int thumb_off = max_scroll ? off * travel / max_scroll : 0;
    kui_fill(c, x + thumb_off, y, thumb, bar, true);
}

/* The reference box_rounded: a rectangle with its four corner pixels cut to paper. */
static void box_rounded(kui_canvas_t *c, int x, int y, int w, int h, bool filled) {
    if (w < 2 || h < 2) return;
    if (filled) kui_fill(c, x, y, w, h, true);
    else kui_frame(c, x, y, w, h, true);
    kui_pixel(c, x, y, false);
    kui_pixel(c, x + w - 1, y, false);
    kui_pixel(c, x, y + h - 1, false);
    kui_pixel(c, x + w - 1, y + h - 1, false);
}

static void draw_icon(kui_canvas_t *c, const kui_item_t *e, int x, int y, int w, int h, bool on) {
    if (e && e->bits && e->w > 0 && e->h > 0) kui_bitmap_scaled(c, e->bits, e->w, e->h, x, y, w, h, on);
}

static const char *label_of(const kui_item_t *e) { return e && e->label ? e->label : ""; }

static int clamp_cursor(int cursor, int n) {
    if (cursor < 0) return 0;
    if (cursor >= n) return n - 1;
    return cursor;
}

/* ---- strip: the strip carousel launcher (9471f26 ui_launcher.c), in logical pixels. The device name
 * top left; the focused app as a 48 px tile with its 32 px icon and an inverted Launch band; its name beside the tile under
 * the row; the next apps as 32 px tiles in a tight row (16 px icons); page dots at the bottom. ---- */

#define PS_BIG 48     /* the focused tile */
#define PS_SMALL 32   /* the neighbours */
#define PS_GAP 4      /* between tiles */
#define PS_BIG_X 38   /* the focused tile's left edge: a left neighbour fits at x = 2 */
#define PS_BAND_H 9   /* the Launch band inside the tile's bottom */
#define PS_DOT_UP 8   /* page dots: this far above the area's bottom (y 112 of the 2x screen) */
#define PS_DOT_GAP 6  /* dot pitch */

/* Draws one 32 px tile outlined, with its 16 px icon centred. */
static void ps_small(kui_canvas_t *c, int x, int y, const kui_item_t *e) {
    kui_round(c, x, y, PS_SMALL, PS_SMALL, 2, true);
    if (e && e->small_bits && e->small_w > 0)
        kui_bitmap(c, x + (PS_SMALL - e->small_w) / 2, y + (PS_SMALL - e->small_h) / 2, e->small_w, e->small_h, e->small_bits,
                   true);
}

/* Copies s into out, cut so it fits w pixels in font f (the names are short; a cut name has no ellipsis). */
static const char *ps_fit(const kui_font_t *f, const char *s, int w, char *out, int n) {
    int i = 0;
    for (; s[i] && i < n - 1; i++) out[i] = s[i];
    out[i] = 0;
    while (i > 0 && kui_text_w(f, out) > w) out[--i] = 0;
    return out;
}

void kui_skin_strip(kui_canvas_t *c, const char *title, const kui_item_t *it, int n, int cursor) {
    if (n <= 0) return;
    const kui_font_t *body = kui_slot(KUI_F_REG8), *sub = kui_slot(KUI_F_BOLD8);
    const int W = c->w, H = c->h, top = c->clip_y0;
    cursor = clamp_cursor(cursor, n);
    kui_fill(c, 0, top, W, H - top, false); /* the area is repainted from scratch */

    kui_text(c, body, 2, top + 2, (title && *title) ? title : "PALANU", true);

    /* the focused tile sits centred in the band between the title and the dots */
    const int dot_y = c->clip_y1 - PS_DOT_UP; /* follows the area, so the dots stay at the bottom at any UI scale */
    int band_top = top + 12, band_bot = dot_y - 4;
    int big_top = band_top + (band_bot - band_top - PS_BIG) / 2;
    if (big_top < band_top) big_top = band_top;
    int big_x = PS_BIG_X;
    int small_top = big_top + 2; /* the neighbours sit high, the name fits below them */

    if (cursor > 0) ps_small(c, big_x - PS_GAP - PS_SMALL, small_top, &it[cursor - 1]);

    /* the focused tile: an outlined rounded frame with a second line inside, the icon, and the Launch band */
    kui_round(c, big_x, big_top, PS_BIG, PS_BIG, 2, true);
    kui_round(c, big_x + 1, big_top + 1, PS_BIG - 2, PS_BIG - 2, 1, true);
    const kui_item_t *e = &it[cursor];
    int band_y = big_top + PS_BIG - 2 - PS_BAND_H;
    int icon_y = big_top + 3 + (band_y - 2 - (big_top + 3) - 32) / 2;
    if (e->bits && e->w > 0) kui_bitmap_scaled(c, e->bits, e->w, e->h, big_x + (PS_BIG - 32) / 2, icon_y, 32, 32, true);
    kui_fill(c, big_x + 2, band_y, PS_BIG - 4, PS_BAND_H, true);
    const char *act = "Launch";
    kui_text(c, body, big_x + (PS_BIG - kui_text_w(body, act)) / 2, band_y + (PS_BAND_H - body->charH) / 2, act, false);

    /* the focused app's name to the right of the tile, under the neighbours */
    int nx = big_x + PS_BIG + PS_GAP;
    int nw = W - nx - 2;
    char name[40];
    int name_y = small_top + PS_SMALL + 3; /* 3 px under the neighbours' bottom edge, inside the tile */
    if (nw > 0) kui_text(c, sub, nx, name_y, ps_fit(sub, label_of(e), nw, name, sizeof(name)), true);

    /* the next apps in a tight row; the last one may be cut by the screen edge */
    int x = nx;
    for (int i = cursor + 1; i < n && x < W; i++) {
        ps_small(c, x, small_top, &it[i]);
        x += PS_SMALL + PS_GAP;
    }

    /* page dots: the current one filled, the others outlined; a counter past 24 pages */
    if (n <= 24) {
        int dw = (n - 1) * PS_DOT_GAP + 3, dx = (W - dw) / 2;
        for (int i = 0; i < n; i++) {
            int px = dx + i * PS_DOT_GAP;
            if (i == cursor) kui_fill(c, px, dot_y, 3, 3, true);
            else kui_frame(c, px, dot_y, 3, 3, true);
        }
    } else {
        char pos[16];
        snprintf(pos, sizeof(pos), "%d/%d", cursor + 1, n);
        kui_text(c, body, (W - kui_text_w(body, pos)) / 2, dot_y, pos, true);
    }
}

/* ---- grid: the channel grid. Two columns of rounded channel tiles with a 1 px shadow; the focused one is inverted. */

static void wii_channel(kui_canvas_t *c, int x, int y, int w, int h, const kui_item_t *e, bool sel) {
    box_rounded(c, x + 1, y + 1, w, h, true); /* shadow */
    if (sel) {
        box_rounded(c, x, y, w, h, true);
    } else {
        kui_fill(c, x, y, w, h, false);
        box_rounded(c, x, y, w, h, false);
    }
    const kui_font_t *cap = &FONT_REG8;
    int lbl_h = cap->charH;
    if (e && e->bits && e->w > 0 && e->h > 0) {
        int band_h = h > lbl_h + 3 ? h - lbl_h - 3 : h;
        int isz = band_h * 4 / 5;
        int lim = w * 3 / 4;
        if (isz > lim) isz = lim;
        int ix = x + (w > isz ? (w - isz) / 2 : 0);
        int iy = y + (band_h > isz ? (band_h - isz) / 2 : 1);
        draw_icon(c, e, ix, iy, isz, isz, !sel);
    }
    const char *lab = label_of(e);
    int lw = kui_text_w(cap, lab);
    int lx = x + (w > lw ? (w - lw) / 2 : 0);
    int ly = y + h - lbl_h - 1;
    kui_text(c, cap, lx, ly, lab, !sel);
}

void kui_skin_grid(kui_canvas_t *c, const kui_item_t *it, int n, int cursor) {
    if (n <= 0) return;
    const int cols = 2, gap = SPACE_SM, sb_w = 4, botpad = SPACE_LG;
    const int W = c->w, H = c->h, top = c->clip_y0;
    cursor = clamp_cursor(cursor, n);

    int area_h = H > top + botpad ? H - top - botpad : 0;
    int grid_w = W > sb_w ? W - sb_w : W;
    int tile_w = (grid_w - gap * (cols + 1)) / cols;
    int tile_h = tile_w * 3 / 4;
    if (area_h > gap * 3) {
        int max_h2 = (area_h - gap * 3) / 2;
        if (tile_h > max_h2) tile_h = max_h2;
    }
    int row_h = tile_h + gap;

    int rows = (n + cols - 1) / cols;
    int cur_row = cursor / cols;
    int vis_rows = row_h ? area_h / row_h : 1;
    if (vis_rows < 1) vis_rows = 1;
    int first_row = 0;
    if (cur_row >= vis_rows) first_row = cur_row - vis_rows + 1;

    for (int i = 0; i < n; i++) {
        int r = i / cols, col = i % cols;
        int vr = r - first_row;
        if (vr < 0 || vr >= vis_rows) continue;
        int tx = gap + col * (tile_w + gap);
        int ty = top + vr * row_h;
        wii_channel(c, tx, ty, tile_w, tile_h, &it[i], i == cursor);
    }
    if (rows > vis_rows) kui_scrollbar(c, W - sb_w + 1, top, area_h, first_row * row_h, vis_rows * row_h, rows * row_h);
}

/* ---- compact: the 3D card stack. The focused card is a flat square; its neighbours show as the side faces of cards tucked
 * behind it. A position bar sits at the bottom, the name under the square. ---- */

/* The reference drawSideCard: the isometric hexagon of one side card, with its icon sheared down the face. */
static void cmp_side(kui_canvas_t *c, int outer_x, int inner_x, int yt, int yb, int skew, bool left_card, const kui_item_t *e) {
    int inner_yt = yt + skew;
    int inner_yb = yb > skew ? yb - skew : yt + 2;
    if (inner_yb <= inner_yt) return;

    kui_line(c, outer_x, yt, outer_x, yb, true);
    kui_line(c, inner_x, inner_yt, inner_x, inner_yb, true);
    kui_line(c, outer_x, yt, inner_x, inner_yt, true);
    kui_line(c, outer_x, yb, inner_x, inner_yb, true);

    int face_l = left_card ? outer_x : inner_x;
    int face_r = left_card ? inner_x : outer_x;
    int face_w = face_r - face_l;
    int inner_h = inner_yb - inner_yt;
    if (!e || !e->bits || e->w <= 0 || e->h <= 0 || face_w < 5 || inner_h < 5) return;

    int isz = (face_w < inner_h ? face_w : inner_h) * 3 / 5;
    if (isz < 2) return;

    int icon_x = face_l + (face_w - isz) / 2;
    int mid_y = (inner_yt + inner_yb) / 2;
    int icon_y = mid_y > isz / 2 ? mid_y - isz / 2 : 0;

    for (int dx = 0; dx < isz; dx++) {
        int sx = dx * e->w / isz;
        if (sx >= e->w) sx = e->w - 1;
        int shear = left_card ? dx * skew / isz : (isz - 1 - dx) * skew / isz;
        for (int dy = 0; dy < isz; dy++) {
            int sy = dy * e->h / isz;
            if (sy >= e->h) sy = e->h - 1;
            if (src_bit(e->bits, e->w, sx, sy)) kui_pixel(c, icon_x + dx, icon_y + dy + shear, true);
        }
    }
}

void kui_skin_compact(kui_canvas_t *c, const kui_item_t *it, int n, int cursor) {
    if (n <= 0) return;
    const int W = c->w, H = c->h, top = c->clip_y0;
    cursor = clamp_cursor(cursor, n);
    const kui_font_t *sub = &FONT_BOLD8;

    int sb_h = SPACE_SM;
    int label_h = sub->charH;
    int bot_y = H - sb_h - label_h - 2;
    int band_h = bot_y > top ? bot_y - top : 0;
    if (band_h < 8) return;

    int s = band_h;
    int max_s = W * 2 / 5;
    if (s > max_s) s = max_s;
    int cx = (W - s) / 2;
    int cy = top + (band_h - s) / 2;
    int yt = cy, yb = cy + s;

    int sw = s * 40 / 100; /* face width of each side card */
    int skew = s * 25 / 100; /* top and bottom taper of the inner edge */

    {
        int inner_x = cx + s;
        for (int i = cursor + 1; i < n; i++) {
            if (inner_x >= W - 2) break;
            int outer_x = inner_x + sw;
            if (outer_x > W - 1) outer_x = W - 1;
            if (outer_x - inner_x < 4) break;
            cmp_side(c, outer_x, inner_x, yt, yb, skew, false, &it[i]);
            inner_x = outer_x;
        }
    }
    {
        int inner_x = cx;
        for (int i = cursor - 1; i >= 0; i--) {
            if (inner_x < sw + 2) break;
            int outer_x = inner_x - sw;
            if (inner_x - outer_x < 4) break;
            cmp_side(c, outer_x, inner_x, yt, yb, skew, true, &it[i]);
            inner_x = outer_x;
        }
    }

    /* the focused card, painted last so it covers the side faces */
    kui_fill(c, cx, cy, s, s, false);
    box_rounded(c, cx, cy, s, s, false);
    {
        const kui_item_t *e = &it[cursor];
        if (e->bits && e->w > 0 && e->h > 0 && s > 6) {
            int isz = s * 3 / 5;
            draw_icon(c, e, cx + (s - isz) / 2, cy + (s - isz) / 2, isz, isz, true);
        }
    }

    {
        const char *nm = label_of(&it[cursor]);
        int lw = kui_text_w(sub, nm);
        int lx = W > lw ? (W - lw) / 2 : 0;
        kui_text(c, sub, lx, bot_y + 1, nm, true);
    }

    kui_hscrollbar(c, 2, H - sb_h, W - 4, cursor, 1, n);
}

/* ---- list: one icon and label per row, the selected row inverted; the list scrolls to keep the cursor centred. */

void kui_skin_list(kui_canvas_t *c, const kui_item_t *it, int n, int cursor) {
    if (n <= 0) return;
    const int W = c->w, H = c->h, top = c->clip_y0;
    cursor = clamp_cursor(cursor, n);
    const kui_font_t *body = &FONT_REG8;
    const int sb_w = 3, pad_x = 3, gap = 3;

    int area = H > top ? H - top : 0;
    if (area < 4) return;
    int text_h = body->charH;
    int row_h = text_h + 4; /* 2 px above and below */
    int icon_sz = row_h > 4 ? row_h - 4 : 1;
    int row_w = W > sb_w + 1 ? W - sb_w - 1 : W;

    int vis = area / row_h > 0 ? area / row_h : 1;
    int scroll = cursor - vis / 2;
    if (scroll > n - vis) scroll = n - vis;
    if (scroll < 0) scroll = 0;

    for (int i = scroll; i < scroll + vis && i < n; i++) {
        bool sel = i == cursor;
        int row_y = top + (i - scroll) * row_h;
        if (sel) box_rounded(c, 0, row_y, row_w, row_h, true);
        bool fg = !sel;
        const kui_item_t *e = &it[i];
        draw_icon(c, e, pad_x, row_y + (row_h - icon_sz) / 2, icon_sz, icon_sz, fg);
        kui_text(c, body, pad_x + icon_sz + gap, row_y + (row_h - text_h) / 2, label_of(e), fg);
    }

    if (n > vis) kui_scrollbar(c, W - sb_w, top, area, scroll, vis, n);
}
