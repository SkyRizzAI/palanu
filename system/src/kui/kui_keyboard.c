/* kui keyboard keys: the reference VirtualKeyboard drawKey and drawCapsKey.
 * A key is a cell of the grid with no gap: a selected key is a filled box with rounded corners (radius 2) and its label in
 * paper; otherwise the label is ink. Labels are FONT_6X8, centred at the reference CHAR_W of 6 px per character. */
#include <string.h>

#include "pal/kui.h"

#define KEY_CHAR_W 6 /* The reference ui_constants CHAR_W: 5 px glyph and 1 px spacing */

void kui_key(kui_canvas_t *c, int x, int y, int w, int h, const char *label, bool sel) {
    if (sel) kui_fill_round(c, x, y, w, h, 2, true);
    int lw = (int)strlen(label) * KEY_CHAR_W;
    kui_text(c, &FONT_REG8, x + (w - lw) / 2, y + (h - 8) / 2, label, !sel);
}
