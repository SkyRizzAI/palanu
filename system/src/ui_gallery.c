/* A contact sheet of every icon at its three sizes (dev only; `uishow gallery`). Used to review the pixel art on the screen
 * and in the golden images. Hidden from the launcher. */
#include <string.h>

#include "pal/icons.h"
#include "pal/ui.h"

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_PAPER);
    int col = 0, x = a.x + 4, y = a.y + 4, rowh = 56;
    for (size_t i = 0; i < PAL_ICONS_COUNT; i++) {
        const pal_icon_t *ic = &PAL_ICONS[i];
        if (ic->size != 32) continue;
        if (col == 8) {
            col = 0;
            x = a.x + 4;
            y += rowh;
        }
        pal_icon_blit(c, ic, x + (36 - 32) / 2, y, PAL_C_INK, PAL_C_ACCENT);
        const pal_icon_t *small = pal_icon_find(ic->name, 16);
        if (small) pal_icon_blit(c, small, x + 10, y + 34, PAL_C_INK, PAL_C_ACCENT);
        col++;
        x += 38;
    }
    y += rowh + 6;
    x = a.x + 4;
    for (size_t i = 0; i < PAL_ICONS_COUNT; i++) {
        const pal_icon_t *ic = &PAL_ICONS[i];
        if (ic->size != 8) continue;
        pal_icon_blit(c, ic, x, y, PAL_C_INK, PAL_C_ACCENT);
        x += 14;
    }
    pal_text(c, t->tokens.caption, a.x + 4, a.y + a.h - t->tokens.caption->h - 4, "32px, 16px under, 8px row", PAL_C_DIM,
             PAL_TRANSPARENT);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v, (void)k;
    return false;
}

pal_view_t pal_gallery_view = {
    .id = "gallery", .title = "Icons", .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_HIDDEN, .draw = draw, .key = key,
};
