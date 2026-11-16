/* PalanuOS canvas: an 8-bit indexed framebuffer with drawing primitives and dirty tracking.
 *
 * Why 8-bit indexed: terminal programs use colour, the framebuffer lives in PSRAM so its 76.8 KB costs no
 * internal RAM, and a palette lets themes and dark mode recolour everything without redrawing logic.
 * Every primitive extends a dirty rectangle, so the display only sends what changed.
 */
#ifndef PAL_GFX_H
#define PAL_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/font.h"

#define PAL_TRANSPARENT (-1)
typedef int pal_color_t; /* palette index 0..255, or PAL_TRANSPARENT */

typedef struct {
    uint8_t *px;
    int w, h, stride;
    int clip_x0, clip_y0, clip_x1, clip_y1; /* exclusive upper bounds */
    int dirty_x0, dirty_y0, dirty_x1, dirty_y1; /* empty when x1 <= x0 */
} pal_canvas_t;

typedef struct {
    int x, y, w, h;
} pal_rect_t;

void pal_canvas_init(pal_canvas_t *c, void *pixels, int w, int h);
void pal_clip_set(pal_canvas_t *c, int x, int y, int w, int h);
void pal_clip_reset(pal_canvas_t *c);

/* Dirty rectangle. take returns false if nothing changed; otherwise fills r and clears it. */
void pal_mark(pal_canvas_t *c, int x, int y, int w, int h);
void pal_mark_all(pal_canvas_t *c);
bool pal_take_dirty(pal_canvas_t *c, pal_rect_t *r);

void pal_clear(pal_canvas_t *c, pal_color_t color);
void pal_pixel(pal_canvas_t *c, int x, int y, pal_color_t color);
pal_color_t pal_get(const pal_canvas_t *c, int x, int y);
void pal_fill(pal_canvas_t *c, int x, int y, int w, int h, pal_color_t color);
void pal_rect(pal_canvas_t *c, int x, int y, int w, int h, pal_color_t color); /* outline */
void pal_line(pal_canvas_t *c, int x0, int y0, int x1, int y1, pal_color_t color);
void pal_circle(pal_canvas_t *c, int cx, int cy, int r, pal_color_t color);
void pal_fill_circle(pal_canvas_t *c, int cx, int cy, int r, pal_color_t color);
/* Rounded rectangles in pixel-art style: radius 1 clips only the four corner pixels. */
void pal_fill_round(pal_canvas_t *c, int x, int y, int w, int h, int r, pal_color_t color);
void pal_round(pal_canvas_t *c, int x, int y, int w, int h, int r, pal_color_t color);

/* 1-bit bitmap (rows padded to bytes, MSB first). bg may be PAL_TRANSPARENT. */
void pal_blit_mono(pal_canvas_t *c, int x, int y, int w, int h, const uint8_t *bits, int bpr, pal_color_t fg,
                   pal_color_t bg);
/* Copy a rectangle of 8-bit pixels. */
void pal_blit(pal_canvas_t *c, int x, int y, int w, int h, const uint8_t *src, int src_stride);

/* Text. Returns the x position after the last glyph. */
int pal_text(pal_canvas_t *c, const pal_font_t *f, int x, int y, const char *utf8, pal_color_t fg, pal_color_t bg);
int pal_text_w(const pal_font_t *f, const char *utf8);
/* Draw one cell-aligned glyph by code point. */
void pal_glyph(pal_canvas_t *c, const pal_font_t *f, int x, int y, uint32_t cp, pal_color_t fg, pal_color_t bg);

#endif
