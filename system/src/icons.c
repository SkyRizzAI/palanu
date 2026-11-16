#include <stdio.h>
#include <string.h>

#include "pal/icons.h"
#include "pal/theme.h"

const pal_icon_t *pal_icon_find(const char *name, int size) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < PAL_ICONS_COUNT; i++)
        if (PAL_ICONS[i].size == size && strcmp(PAL_ICONS[i].name, name) == 0) return &PAL_ICONS[i];
    return NULL;
}

void pal_icon_blit(pal_canvas_t *c, const pal_icon_t *ic, int x, int y, pal_color_t ink, pal_color_t accent) {
    if (!ic) return;
    int bpr = (ic->w + 7) / 8;
    for (int r = 0; r < ic->h; r++) {
        for (int col = 0; col < ic->w; col++) {
            int bit = 0x80 >> (col & 7);
            int idx = r * bpr + (col >> 3);
            if (ic->bits[idx] & bit) pal_pixel(c, x + col, y + r, ink);
            else if (ic->accent && (ic->accent[idx] & bit)) pal_pixel(c, x + col, y + r, accent);
        }
    }
}

/* The vector shapes of the first version, kept for ids without a bitmap. All shapes are laid out on a 24-unit grid. */
#define U(v) ((v) * size / 24)

static void gear(pal_canvas_t *c, int x, int y, int size, pal_color_t fg) {
    int cx = x + size / 2, cy = y + size / 2;
    static const int8_t dx[8] = {0, 7, 10, 7, 0, -7, -10, -7};
    static const int8_t dy[8] = {-10, -7, 0, 7, 10, 7, 0, -7};
    for (int i = 0; i < 8; i++) pal_fill_circle(c, cx + U(dx[i]), cy + U(dy[i]), U(2), fg);
    pal_fill_circle(c, cx, cy, U(8), fg);
    pal_fill_circle(c, cx, cy, U(3), PAL_C_PANEL);
}

static void terminal(pal_canvas_t *c, int x, int y, int size, pal_color_t fg) {
    pal_round(c, x + U(2), y + U(3), U(20), U(18), 2, fg);
    pal_fill(c, x + U(3), y + U(4), U(18), 2, fg);
    int cx = x + U(6), cy = y + U(12);
    pal_line(c, cx, cy - U(3), cx + U(3), cy, fg);
    pal_line(c, cx + U(3), cy, cx, cy + U(3), fg);
    pal_fill(c, x + U(11), y + U(15), U(6), 2, fg);
}

static void monitor(pal_canvas_t *c, int x, int y, int size, pal_color_t fg) {
    pal_round(c, x + U(2), y + U(3), U(20), U(14), 2, fg);
    pal_fill(c, x + U(11), y + U(17), U(2), U(3), fg);
    pal_fill(c, x + U(7), y + U(20), U(10), 2, fg);
}

static void files(pal_canvas_t *c, int x, int y, int size, pal_color_t fg) {
    pal_round(c, x + U(2), y + U(6), U(20), U(15), 2, fg);
    pal_fill(c, x + U(2), y + U(4), U(8), U(3), fg);
}

static void link_icon(pal_canvas_t *c, int x, int y, int size, pal_color_t fg) {
    pal_fill_circle(c, x + U(12), y + U(12), U(2), fg);
    for (int r = 5; r <= 11; r += 3) pal_circle(c, x + U(12), y + U(12), U(r), fg);
}

static const struct {
    const char *id;
    void (*draw)(pal_canvas_t *, int, int, int, pal_color_t);
} VECTOR[] = {{"terminal", terminal}, {"monitor", monitor}, {"settings", gear}, {"files", files}, {"link", link_icon}};

static int size_class(int size) { return size >= 24 ? 32 : size >= 12 ? 16 : 8; }

void pal_icon_draw(pal_canvas_t *c, const char *id, const char *title, int x, int y, int size, pal_color_t fg, pal_color_t bg) {
    (void)bg;
    const char *name = id ? id : "";
    const pal_icon_t *ic = pal_icon_find(name, size_class(size));
    if (!ic) ic = pal_icon_find(name, 32); /* a name with only one size: draw it at its own size */
    if (!ic) ic = pal_icon_find(name, 16);
    if (!ic) ic = pal_icon_find(name, 8);
    if (ic) {
        pal_icon_blit(c, ic, x + (size - ic->w) / 2, y + (size - ic->h) / 2, fg, PAL_C_ACCENT);
        return;
    }
    for (size_t i = 0; i < sizeof(VECTOR) / sizeof(VECTOR[0]); i++)
        if (strcmp(VECTOR[i].id, name) == 0) {
            VECTOR[i].draw(c, x, y, size, fg);
            return;
        }
    (void)title;
    ic = pal_icon_find("default", size_class(size));
    if (!ic) ic = pal_icon_find("default", 32);
    if (ic) pal_icon_blit(c, ic, x + (size - ic->w) / 2, y + (size - ic->h) / 2, fg, PAL_C_ACCENT);
}
