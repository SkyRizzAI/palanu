// Shared LCD driver for the hhv2 board bring-up tests (ILI9341 320x240, SPI2).
// Settings confirmed in 01_lcd: swap_xy + mirror(x,y) + colour invert, BGR order.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define HHV2_LCD_W 320
#define HHV2_LCD_H 240

// Plain RGB565 colour (not swapped; the driver swaps bytes when sending)
#define HHV2_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Initialises the SPI bus, the panel and the backlight. SD CS (GPIO5) is held HIGH.
void hhv2_lcd_init(void);

void hhv2_lcd_backlight(bool on);

// Fills a rectangle with one colour
void hhv2_lcd_fill(int x, int y, int w, int h, uint16_t color);

// Draws text in the 5x7 font (capitals, digits, a few symbols). Lower case
// is drawn as capitals. The text background is filled with colour bg. Height = 7*scale.
void hhv2_lcd_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg);

// Width in pixels of a text at a given scale
int hhv2_lcd_text_w(const char *s, int scale);

// 5x7 glyph data: 7 rows, bit4 = leftmost column
const uint8_t *hhv2_font_glyph(char c);
