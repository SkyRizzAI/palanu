/* kui text: the reference glyph renderer (drawChar, drawText, textWidth) and glyph lookup. */
#include "pal/kui.h"

static inline uint8_t glyph_bpc(const kui_font_t *f) { return f->bytesPerCol ? f->bytesPerCol : 1; }

static inline uint8_t glyph_w(const kui_font_t *f, uint8_t idx) { return f->widths ? f->widths[idx] : f->charW; }

static inline const uint8_t *glyph_data(const kui_font_t *f, uint8_t idx) {
    return f->offsets ? f->data + f->offsets[idx] : f->data + (unsigned)idx * f->charW * glyph_bpc(f);
}

/* Width of one character in the font, or the fallback advance for a character outside the font. */
static inline int char_adv(const kui_font_t *f, unsigned char ch) {
    if (ch >= f->firstChar && (unsigned)(ch - f->firstChar) < f->numChars) return glyph_w(f, (uint8_t)(ch - f->firstChar));
    return f->charW;
}

/* The reference drawChar: each column is scanned for runs of lit rows, and every run is one filled 1 px wide rectangle. */
void kui_char(kui_canvas_t *c, const kui_font_t *f, int x, int y, char ch, bool on) {
    unsigned char uc = (unsigned char)ch;
    if (uc < f->firstChar) return;
    unsigned idx = uc - f->firstChar;
    if (idx >= f->numChars) return;
    const uint8_t *glyph = glyph_data(f, (uint8_t)idx);
    int gw = glyph_w(f, (uint8_t)idx), bpc = glyph_bpc(f);
    for (int col = 0; col < gw; col++) {
        const uint8_t *cb = glyph + col * bpc;
        int row = 0;
        while (row < f->charH) {
            while (row < f->charH && !(cb[row >> 3] & (1 << (row & 7)))) row++;
            if (row >= f->charH) break;
            int start = row;
            while (row < f->charH && (cb[row >> 3] & (1 << (row & 7)))) row++;
            kui_fill(c, x + col, y + start, 1, row - start, on);
        }
    }
}

/* The reference drawText: stops at the canvas edge; spacing comes after every glyph. */
void kui_text(kui_canvas_t *c, const kui_font_t *f, int x, int y, const char *s, bool on) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        int gw = char_adv(f, (unsigned char)*p);
        if (cx + gw > c->w) break;
        kui_char(c, f, cx, y, *p, on);
        cx += gw + f->spacing;
    }
}

/* The reference textWidth: the sum of advances without the trailing spacing. */
int kui_text_w(const kui_font_t *f, const char *s) {
    int w = 0;
    for (const char *p = s; *p; p++) w += char_adv(f, (unsigned char)*p) + f->spacing;
    return w ? w - f->spacing : 0;
}

/* The reference drawChar at an integer scale: every lit logical pixel of the glyph is a scale x scale block (the big clock). */
void kui_text_scaled(kui_canvas_t *c, const kui_font_t *f, int x, int y, const char *s, int scale, bool on) {
    if (scale < 1) scale = 1;
    int cx = x;
    for (const char *p = s; *p; p++) {
        unsigned char uc = (unsigned char)*p;
        int adv = char_adv(f, uc);
        if (uc < f->firstChar || (unsigned)(uc - f->firstChar) >= f->numChars) {
            cx += (adv + f->spacing) * scale;
            continue;
        }
        unsigned idx = uc - f->firstChar;
        const uint8_t *glyph = glyph_data(f, (uint8_t)idx);
        int gw = glyph_w(f, (uint8_t)idx), bpc = glyph_bpc(f);
        for (int col = 0; col < gw; col++) {
            const uint8_t *cb = glyph + col * bpc;
            for (int row = 0; row < f->charH; row++)
                if (cb[row >> 3] & (1 << (row & 7))) kui_fill(c, cx + col * scale, y + row * scale, scale, scale, on);
        }
        cx += (adv + f->spacing) * scale;
    }
}

/* Width of kui_text_scaled's output: kui_text_w at the scale, without the trailing spacing. */
int kui_text_w_scaled(const kui_font_t *f, const char *s, int scale) { return kui_text_w(f, s) * scale; }
