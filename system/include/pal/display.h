/* PalanuOS display: owns the framebuffer and hands changed rectangles to the board's panel driver. */
#ifndef PAL_DISPLAY_H
#define PAL_DISPLAY_H

#include "nema/types.h"
#include "pal/gfx.h"

typedef struct {
    int width, height;
    /* Send the rectangle r of the 8-bit framebuffer to the panel, expanding through the palette
     * (256 RGB565 values, already byte-swapped for SPI panels). Blocks until the rectangle has been sent. */
    void (*present)(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *palette);
    void (*backlight)(void *ctx, int percent); /* may be NULL */
    void *ctx;
} pal_display_ops_t;

nema_err_t pal_display_init(const pal_display_ops_t *ops);
pal_canvas_t *pal_display_canvas(void);
/* Send whatever changed since the last flush. Returns the number of pixels sent. */
int pal_display_flush(void);
/* Rebuild the palette from the active theme and redraw everything. */
void pal_display_refresh_theme(void);
void pal_display_set_brightness(int percent);
/* Backlight on or off, keeping the brightness setting: the display power manager uses it for sleep. */
void pal_display_backlight(bool on);
int pal_display_brightness(void);
uint32_t pal_display_flush_count(void);
const uint16_t *pal_display_palette(void); /* 256 entries, RGB565 with the bytes swapped (panel bus order) */
/* Called after each flush with the rectangle that was presented; for remote screen mirroring. One tap. */
void pal_display_set_tap(void (*tap)(void *ctx, pal_rect_t r), void *ctx);

#endif
