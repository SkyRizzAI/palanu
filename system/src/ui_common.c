/* Drawing helpers shared by the screens: status chips, sliders, scrollbars and scaled text. Every colour
 * is a theme palette slot, so the helpers look right in every theme and in dark mode. */
#include <stdio.h>
#include <string.h>

#include "pal/icons.h"
#include "pal/ui.h"

static int utf8_cut(const char *s, size_t n) {
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; /* do not split a multi-byte character */
    return (int)n;
}

int pal_ui_text_fit(const pal_font_t *f, const char *text, int maxw, char *out, size_t cap) {
    if (cap == 0) return 0;
    if (pal_text_w(f, text) <= maxw) {
        snprintf(out, cap, "%s", text);
        return pal_text_w(f, out);
    }
    int dots = pal_text_w(f, "..");
    size_t n = strlen(text);
    if (n >= cap) n = cap - 1;
    while (n > 0) {
        n = (size_t)utf8_cut(text, n - 1);
        char tmp[64];
        if (n + 3 > sizeof(tmp)) continue;
        memcpy(tmp, text, n);
        tmp[n] = 0;
        if (pal_text_w(f, tmp) + dots <= maxw) {
            snprintf(out, cap, "%s..", tmp);
            return pal_text_w(f, out);
        }
    }
    snprintf(out, cap, "..");
    return dots;
}

int pal_ui_big_text(pal_canvas_t *c, const pal_font_t *f, int x, int y, const char *text, int scale, pal_color_t fg) {
    for (uint32_t cp; (cp = pal_utf8_next(&text)) != 0;) {
        const uint8_t *g = pal_font_glyph(f, cp);
        int bpr = pal_font_bpr(f);
        for (int r = 0; r < f->h; r++)
            for (int col = 0; col < f->w; col++)
                if (g[r * bpr + (col >> 3)] & (0x80 >> (col & 7)))
                    pal_fill(c, x + col * scale, y + r * scale, scale, scale, fg);
        x += f->w * scale;
    }
    return x;
}

void pal_ui_box(pal_canvas_t *c, int x, int y, int w, int h, int r, pal_color_t fill, pal_color_t edge) {
    pal_fill_round(c, x, y, w, h, r, fill);
    if (edge >= 0) pal_round(c, x, y, w, h, r, edge);
}

int pal_ui_chip(pal_canvas_t *c, const pal_theme_t *t, int x, int y, const char *icon, const char *label, bool on) {
    const pal_font_t *f = t->tokens.caption;
    int h = f->h + 4;
    int pad = 3;
    int w = pad + 8 + 3 + pal_text_w(f, label) + pad;
    pal_color_t bg = on ? PAL_C_INK : PAL_C_PANEL;
    pal_color_t fg = on ? PAL_C_PAPER : PAL_C_INK;
    pal_fill_round(c, x, y, w, h, 3, bg);
    const pal_icon_t *ic = pal_icon_find(icon, 8);
    if (ic) pal_icon_blit(c, ic, x + pad, y + (h - 8) / 2, fg, PAL_C_ACCENT);
    pal_text(c, f, x + pad + 8 + 3, y + 2, label, fg, PAL_TRANSPARENT);
    return w;
}

void pal_ui_vslider(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int w, int h, int value, int max, bool focused,
                    bool enabled) {
    (void)t;
    pal_fill_round(c, x, y, w, h, w / 2, PAL_C_PANEL);
    pal_round(c, x, y, w, h, w / 2, focused ? PAL_C_INK : PAL_C_DIM);
    if (focused) pal_round(c, x + 1, y + 1, w - 2, h - 2, (w - 2) / 2, PAL_C_INK);
    if (max > 0 && value > 0) {
        if (value > max) value = max;
        int fh = (h - 4) * value / max;
        if (fh < 2) fh = 2;
        pal_fill_round(c, x + 2, y + h - 2 - fh, w - 4, fh, (w - 4) / 2, enabled ? PAL_C_INK : PAL_C_DIM);
    }
}

void pal_ui_scrollbar(pal_canvas_t *c, int x, int y, int h, int offset, int visible, int total) {
    if (total <= visible || h <= 0) return;
    for (int i = 0; i < h; i += 2) pal_pixel(c, x + 1, y + i, PAL_C_DIM);
    int th = h * visible / total;
    if (th < 6) th = 6;
    int ty = y + (h - th) * offset / (total - visible);
    pal_fill(c, x, ty, 3, th, PAL_C_INK);
}
