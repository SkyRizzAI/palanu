/* kui desktop: the wallpaper blit of the reference desktop. The reference footer legends are the legend bar now (kui_legend.c, pal/legend.h). Logical canvas,
 * as the other kui files. */
#include "pal/kui.h"

/* The reference anchor fractions: column 0 near edge, 1 centre, 2 far edge; row likewise. Anchor index is row-major. */
static void anchor_frac(int a, float *fx, float *fy) {
    int col = a % 3, row = a / 3;
    *fx = col == 0 ? 0.0f : col == 1 ? 0.5f : 1.0f;
    *fy = row == 0 ? 0.0f : row == 1 ? 0.5f : 1.0f;
}

static inline bool src_bit(const uint8_t *bits, int sw, int sx, int sy) {
    uint32_t idx = (uint32_t)sy * sw + sx;
    return (bits[idx / 8] >> (7 - (idx % 8))) & 1;
}

void kui_wall_fit(kui_canvas_t *c, const uint8_t *bits, int sw, int sh, int tx, int ty, int tw, int th, int fit, int anchor) {
    if (!bits || sw <= 0 || sh <= 0 || tw <= 0 || th <= 0) return;
    float scaleX, scaleY;
    switch (fit) {
    case KUI_FIT_STRETCH:
        scaleX = (float)tw / sw;
        scaleY = (float)th / sh;
        break;
    case KUI_FIT_CROP: { /* cover: uniform max */
        float s = (float)tw / sw, sy = (float)th / sh;
        if (sy > s) s = sy;
        scaleX = scaleY = s;
        break;
    }
    case KUI_FIT_FIT: { /* contain: uniform min */
        float s = (float)tw / sw, sy = (float)th / sh;
        if (sy < s) s = sy;
        scaleX = scaleY = s;
        break;
    }
    case KUI_FIT_CENTER:
    default:
        scaleX = scaleY = 1.0f;
        break;
    }

    float dwF = sw * scaleX, dhF = sh * scaleY;
    float fx, fy;
    anchor_frac(anchor, &fx, &fy);
    float originX = tx + (tw - dwF) * fx;
    float originY = ty + (th - dhF) * fy;

    int x0 = (int)originX;
    if (x0 < tx) x0 = tx;
    int y0 = (int)originY;
    if (y0 < ty) y0 = ty;
    int x1 = (int)(originX + dwF);
    if (x1 > tx + tw) x1 = tx + tw;
    int y1 = (int)(originY + dhF);
    if (y1 > ty + th) y1 = ty + th;

    float invX = 1.0f / scaleX, invY = 1.0f / scaleY;
    for (int y = y0; y < y1; y++) {
        int sy = (int)((y - originY) * invY);
        if (sy < 0) sy = 0;
        else if (sy >= sh) sy = sh - 1;
        for (int x = x0; x < x1; x++) {
            int sx = (int)((x - originX) * invX);
            if (sx < 0) sx = 0;
            else if (sx >= sw) sx = sw - 1;
            kui_pixel(c, x, y, src_bit(bits, sw, sx, sy));
        }
    }
}

