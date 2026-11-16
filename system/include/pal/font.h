/* PalanuOS bitmap fonts: fixed-size cells, 1 bit per pixel, rows padded to whole bytes. */
#ifndef PAL_FONT_H
#define PAL_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint8_t w, h;          /* cell size in pixels */
    uint16_t count;        /* number of glyphs */
    const uint16_t *cps;   /* sorted code points */
    const uint8_t *bits;   /* count * h * ((w + 7) / 8) bytes */
} pal_font_t;

extern const pal_font_t PAL_FONT_SPLEEN_5X8;
extern const pal_font_t PAL_FONT_SPLEEN_6X12;
extern const pal_font_t PAL_FONT_SPLEEN_8X16;

static inline int pal_font_bpr(const pal_font_t *f) { return (f->w + 7) / 8; }

/* Bitmap for a code point; falls back to '?' for anything the font does not carry. */
const uint8_t *pal_font_glyph(const pal_font_t *f, uint32_t cp);
bool pal_font_has(const pal_font_t *f, uint32_t cp);

/* The fallback font (GNU Unifont and a Nerd Font subset, system/src/font_fallback.c): 16 rows of `*cells` bytes (1: 8 pixels wide,
 * 2: 16 wide), MSB first; NULL when neither tier has the code point. The pointer is valid until the next call, and the call is for
 * the desktop task. The CJK and Hangul tier is a file, /data/unifont-cjk.pfnt: without it those return NULL. */
const uint8_t *pal_fallback_glyph(uint32_t cp, int *cells);

/* UTF-8 decoding shared by text drawing and the terminal. Returns the code point and advances *s;
 * malformed input yields U+FFFD. */
uint32_t pal_utf8_next(const char **s);

#endif
