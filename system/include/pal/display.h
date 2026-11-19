/* PalanuOS display: owns the screen frame and hands changed rectangles to the board's panel driver.
 *
 * The screen frame (ADR-0010 P4) is one RGB565 image the size of the panel, in external RAM, owned by the system. Everything that
 * reaches the panel goes through it: the compositor's 8-bit canvas (converted through the theme's palette), the system console (through
 * its own palette) and a full-colour session (copied as it is). Remote mirroring reads it, so it shows every session. A panel driver
 * without present_rgb565 keeps the older path: the 8-bit canvas goes to the panel directly and there is no frame. */
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
    /* Appended later; may be NULL. Send the rectangle r of a full-colour RGB565 image (native byte order, stride in pixels) as it
     * is: a desktop that renders in full colour (Athena) bypasses the 8-bit framebuffer. Blocks until the rectangle has been sent. */
    void (*present_rgb565)(void *ctx, const uint16_t *px, int stride, pal_rect_t r);
    /* Appended later; may be NULL. Turn the panel: 0 or 180 degrees. The image the system draws stays upright (the screen
     * mirror and screenshots too); the panel scans it the other way round. */
    void (*rotate)(void *ctx, int degrees);
} pal_display_ops_t;

nema_err_t pal_display_init(const pal_display_ops_t *ops);
/* 0 or 180 degrees (display.rotation); other values are 0. Everything is sent again. False when the panel cannot turn. */
bool pal_display_set_rotation(int degrees);
int pal_display_rotation(void);
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
/* Full colour (appended later): true when the panel driver takes RGB565 images. present sends the rectangle r of px (screen
 * coordinates, stride in pixels); it returns false and sends nothing when the driver cannot. Desktop task only. */
bool pal_display_has_rgb565(void);
bool pal_display_present_rgb565(const uint16_t *px, int stride, pal_rect_t r);
/* Sends an 8-bit image of the caller's with the caller's palette (byte-swapped RGB565, as pal_theme_palette makes it). The system
 * console draws this way: its pixels and colours are its own, never the theme's or the compositor's canvas. */
void pal_display_present_indexed(const uint8_t *px, int stride, pal_rect_t r, const uint16_t *palette);
/* Rows at the top that a flush of the 8-bit canvas never sends (a session draws its own bar there); 0 by default. */
void pal_display_reserve_top(int rows);
/* The same at the bottom (a session's softkey bar); 0 by default. */
void pal_display_reserve_bottom(int rows);
/* The screen frame: RGB565, native byte order, stride = width; NULL on the older path. Read it on the desktop task (a session copies
 * what is on screen under a modal from it); pal_display_frame_lock guards a reader on another task (mirroring). */
const uint16_t *pal_display_frame(void);
/* Copies an RGB565 image of the caller's (native order, its own stride) into rectangle dst of the screen frame and sends it: an app's
 * canvas (system/src/appui.c). False on the older path (no frame). Desktop task only. */
bool pal_display_blit_rgb565(const uint16_t *src, int stride, pal_rect_t dst);
void pal_display_frame_lock(bool on);

#endif
