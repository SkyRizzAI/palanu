/* kui splash: the reference SplashScreen::draw. The logo is scaled to fit the
 * logical screen (at most 4/5 of its width and half its height, 2:1 aspect) and centred with a progress bar below it, as one
 * block. The bar is a rounded outline with a fill that grows with the progress. Logical canvas, as the other kui files. */
#include "pal/kui.h"

extern const uint8_t KUI_SPLASH_LOGO[1024]; /* kui_splash_logo.c: 128x64 */

#define SPL_LOGO_W 128
#define SPL_LOGO_H 64
#define SPL_BAR_H 5
#define SPL_VGAP 10

void kui_splash_draw(kui_canvas_t *c, float progress) {
    int W = c->w, H = c->h;
    /* the logo: at most 4/5 of the width, and 1/2 of the height keeping the aspect */
    int tw = W * 4 / 5;
    int th = tw * SPL_LOGO_H / SPL_LOGO_W;
    if (th > H / 2) {
        th = H / 2;
        tw = th * SPL_LOGO_W / SPL_LOGO_H;
    }
    if (tw < 8) tw = 8;
    if (th < 4) th = 4;

    /* the logo and the bar are centred as a group */
    int bw = W / 2;
    int blockH = th + SPL_VGAP + SPL_BAR_H;
    int y0 = (H - blockH) / 2;
    if (y0 < 0) y0 = 0;
    int x0 = (W - tw) / 2;

    /* scaled mask blit (the reference blitScaledMask, colour on): nearest neighbour, only the ink pixels are drawn */
    for (int y = 0; y < th; y++) {
        int sy = y * SPL_LOGO_H / th;
        if (sy >= SPL_LOGO_H) sy = SPL_LOGO_H - 1;
        for (int x = 0; x < tw; x++) {
            int sx = x * SPL_LOGO_W / tw;
            if (sx >= SPL_LOGO_W) sx = SPL_LOGO_W - 1;
            int idx = sy * SPL_LOGO_W + sx;
            if (KUI_SPLASH_LOGO[idx >> 3] & (0x80 >> (idx & 7))) kui_pixel(c, x0 + x, y0 + y, true);
        }
    }

    int bx = (W - bw) / 2, by = y0 + th + SPL_VGAP;
    kui_round(c, bx, by, bw, SPL_BAR_H, 2, true);
    int fw = (int)(progress * (float)(bw - 4));
    if (fw > 0) kui_fill_round(c, bx + 2, by + 1, fw, SPL_BAR_H - 2, 1, true);
}
