/* kui legend bar items: an inverted key cap (ink, the key in paper) and its label in ink, as one legend of the bar at the bottom of
 * the screen (pal/legend.h). The label is in FONT_6X8; the key text inside the cap is in FONT_TINY, so "Fn+W" fits a small badge, and
 * a key that is a symbol (KUI_LEGEND_ENTER) is drawn as a small icon. The bar is KUI_LEGEND_H logical rows; the cap is 7 rows of it,
 * centred on the label's 8 rows. Logical coordinates, as the other kui files. */
#include <string.h>

#include "pal/kui.h"

#define CAP_PAD 2   /* paper inside the cap, on each side of the key text */
#define CAP_H 7     /* the cap: the 6 px cell of FONT_TINY plus one row */
#define CAP_R 2     /* the cap's corner inset: a chamfer, as the reference rounded boxes */
#define LABEL_GAP 2 /* between the cap and its label */
#define ICON_W 6    /* the Enter symbol: 5 columns and one of air */
#define ICON_H 5

/* The key text's width inside the cap: FONT_TINY has a column of spacing after each glyph, which the cap's padding replaces. */
static int key_w(const char *key) {
    if (key[0] == KUI_LEGEND_ENTER[0]) return ICON_W - 1;
    int w = kui_text_w(&FONT_TINY, key);
    return w > 0 ? w - 1 : 0;
}

int kui_legend_item_w(const char *key, const char *label) {
    int w = key_w(key) + 2 * CAP_PAD;
    if (label && label[0]) w += LABEL_GAP + kui_text_w(&FONT_6X8, label);
    return w;
}

/* The Enter symbol: a stem down on the right and an arrow back to the left (the return key), in paper on the ink cap. */
static void enter_icon(kui_canvas_t *c, int x, int y) {
    static const char *const ROWS[ICON_H] = {"....#", "....#", ".#..#", "#####", ".#..."};
    for (int r = 0; r < ICON_H; r++)
        for (int col = 0; col < ICON_W - 1; col++)
            if (ROWS[r][col] == '#') kui_pixel(c, x + col, y + r, false);
}

int kui_legend_item(kui_canvas_t *c, int x, int y, const char *key, const char *label) {
    int capW = key_w(key) + 2 * CAP_PAD;
    int capY = y + 2; /* the bar's row 1 is paper, the 8 row label sits in rows 1..8: the 7 row cap in rows 2..8 */
    kui_fill_round(c, x, capY, capW, CAP_H, CAP_R, true);
    if (key[0] == KUI_LEGEND_ENTER[0]) enter_icon(c, x + CAP_PAD, capY + 1);
    else kui_text(c, &FONT_TINY, x + CAP_PAD, capY, key, false);
    int w = capW;
    if (label && label[0]) {
        kui_text(c, &FONT_6X8, x + capW + LABEL_GAP, y + 1, label, true);
        w += LABEL_GAP + kui_text_w(&FONT_6X8, label);
    }
    return w;
}
