/* Private to the terminal emulator (term.c, term_glyph.c). */
#ifndef SYSTEM_TERM_INT_H
#define SYSTEM_TERM_INT_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/font.h"
#include "pal/gfx.h"

/* Draws box drawing and block elements (U+2500-259F) into the cell at x, y (w x h), and the common symbols the font f lacks
 * (squares, circles, triangles, arrows, bullets, check marks). Returns false when the font should draw the code point. */
bool term_special_glyph(pal_canvas_t *c, const pal_font_t *f, int x, int y, int w, int h, uint32_t cp, pal_color_t fg,
                        pal_color_t bg);

/* Columns a code point takes: 0 (combining, joiners, variation selectors), 1, or 2 (wide: CJK, emoji). term_width.c */
int term_cp_width(uint32_t cp);
/* Draws a two-column character across a cell pair w wide: the font's glyph centred when it has one, else an outlined box
 * (the usual "tofu"), so the line keeps its layout. term_glyph.c */
void term_wide_glyph(pal_canvas_t *c, const pal_font_t *f, int x, int y, int w, int h, uint32_t cp, pal_color_t fg, pal_color_t bg);

#endif
