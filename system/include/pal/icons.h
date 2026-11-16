/* PalanuOS icons: pixel-art bitmaps from assets/icons (generated tables, pal/icons_data.h), with the older vector shapes as
 * a fallback for the few ids that have no bitmap. Icon sizes: 32 for launcher tiles, 16 for list rows, 8 for status chips.
 * The accent pixels of an icon ('+' in its source) are drawn in the theme's accent colour. */
#ifndef PAL_ICONS_H
#define PAL_ICONS_H

#include "pal/gfx.h"
#include "pal/icons_data.h"

/* The bitmap called name at the given size (8, 16 or 32), or NULL. */
const pal_icon_t *pal_icon_find(const char *name, int size);
/* Draw a bitmap with its top-left at (x, y). ink is the '#' colour, accent the '+' colour. */
void pal_icon_blit(pal_canvas_t *c, const pal_icon_t *ic, int x, int y, pal_color_t ink, pal_color_t accent);

/* Draw the icon for a name or view id inside the square (x, y, size, size). Order: the bitmap of that id at the size class
 * nearest to size, then the vector shape of that id, then the "default" bitmap. fg is the stroke colour; bg is unused by
 * the bitmaps (the caller paints the tile). */
void pal_icon_draw(pal_canvas_t *c, const char *id, const char *title, int x, int y, int size, pal_color_t fg, pal_color_t bg);

#endif
