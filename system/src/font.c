#include "pal/font.h"

static int find(const pal_font_t *f, uint32_t cp) {
    if (cp > 0xFFFF) return -1;
    int lo = 0, hi = (int)f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->cps[mid] == cp) return mid;
        if (f->cps[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

bool pal_font_has(const pal_font_t *f, uint32_t cp) { return find(f, cp) >= 0; }

const uint8_t *pal_font_glyph(const pal_font_t *f, uint32_t cp) {
    int i = find(f, cp);
    if (i < 0) i = find(f, '?');
    if (i < 0) i = 0;
    return f->bits + (size_t)i * (size_t)f->h * (size_t)pal_font_bpr(f);
}

uint32_t pal_utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p;
    if (c == 0) return 0;
    int extra;
    uint32_t cp;
    if (c < 0x80) { *s += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else { *s += 1; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += (size_t)i; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += (size_t)extra + 1;
    return cp;
}
