/* The hhv2 board (ESP32-S31-WROOM-3): ILI9341 over SPI, TCA8418 keyboard, WS2812, buzzer.
 * Pins and settings come from the bring-up experiments in tests/hw and the pinout in hardware/.
 * The board's capabilities and geometry are in hhv2_board.c (HHV2_BOARD); this header exports the drivers it uses. */
#ifndef HHV2_H
#define HHV2_H

#include "builtin.h"

#define HHV2_LCD_W 320
#define HHV2_LCD_H 240

extern const pal_board_desc_t HHV2_BOARD; /* the descriptor: pass it as pal_boot_cfg_t.board */
extern const pal_keyboard_t HHV2_KEYBOARD;
extern const char *const HHV2_LEGEND_KEYS[]; /* the legend bar's key caps, indexed by pal_legend_action_t (hhv2_legend.c) */

const char *hhv2_storage_init(void);     /* mounts LittleFS; returns its root path or NULL */
const pal_sd_ops_t *hhv2_sd_ops(void);  /* the microSD slot on the LCD's SPI bus (hhv2_sd.c) */
void hhv2_hw_init(void);                 /* backlight, buzzer, RGB LED, SPI bus */
pal_display_ops_t hhv2_display_ops(void); /* the LCD; valid after hhv2_hw_init() */
void hhv2_keyboard_start(void);          /* starts the key scanning task */

/* Board hooks, referenced by HHV2_BOARD. */
void hhv2_beep(int freq_hz, int ms);
void hhv2_rgb(uint8_t r, uint8_t g, uint8_t b);
void hhv2_reboot(void);
int hhv2_battery_percent(void);
int hhv2_battery_mv(void);

#endif
