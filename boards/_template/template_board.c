/* Skeleton of a board port. Not built by default: see boards/_template/README.md for how to copy it and enable it.
 * Only the descriptor and its hooks live here. Drivers for the panel, the keys and the peripherals go in their own
 * files next to this one. Rename TEMPLATE_ to your board's prefix (for example MYBOARD_) when you copy it. */
#include "pal/board.h"
#include "pal/display.h"
#include "pal/input.h"

/* 1. Keys: one entry per physical key, row-major, rows * cols entries. This example is a 1x4 strip. */
static const pal_physkey_t TEMPLATE_KEYS[1 * 4] = {
    {PAL_PK_CHAR, 'a', 0, 0, 0},
    {PAL_PK_CHAR, 's', 0, 0, 0},
    {PAL_PK_CHAR, 'd', 0, 0, 0},
    {PAL_PK_SPECIAL, PAL_KEY_ENTER, 0, 0, 0},
};
static const pal_keyboard_t TEMPLATE_KEYBOARD = {1, 4, TEMPLATE_KEYS};

/* 2. Hooks. Implement only the ones the board has, and set the matching capability bits in the descriptor. The
 * system calls a hook only when its capability is set, so a missing bit means the hook never runs. */
static void template_beep(void *ctx, int freq_hz, int ms) {
    (void)ctx;
    (void)freq_hz;
    (void)ms; /* drive the buzzer for ms milliseconds */
}
static void template_led(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    (void)ctx;
    (void)pixel; /* 0 .. pixels-1 */
    (void)r;
    (void)g;
    (void)b; /* set the status LED */
}
static int template_battery_percent(void) { return -1; /* -1 when unknown */ }
static int template_battery_mv(void) { return -1; /* supply voltage in mV, -1 when unknown */ }
static void template_reboot(void) { /* restart the chip: esp_restart() on ESP-IDF */ }

/* Devices: one entry per physical LED (or strip) and buzzer. Five LEDs are five entries (or one entry with pixels = 5);
 * Settings and the `dev` shell command list and test whatever is declared here. */
static const pal_device_t TEMPLATE_DEVICES[] = {
    {.id = "led0", .name = "Status LED", .kind = PAL_DEV_LED, .led = {.pixels = 1, .colors = 3, .set = template_led}},
    {.id = "buzzer0", .name = "Buzzer", .kind = PAL_DEV_BUZZER, .buzzer = {.beep = template_beep}},
};

/* 3. The descriptor: the one table the system reads. Set a bit only for hardware that is really on the board. */
const pal_board_desc_t TEMPLATE_BOARD = {
    .name = "template",
    .chip = "none",
    .caps = PAL_CAP_DISPLAY | PAL_CAP_KEYBOARD,
    .display_w = 240,
    .display_h = 240,
    .display_format = PAL_PIXFMT_RGB565,
    .keyboard = &TEMPLATE_KEYBOARD,
    .devices = TEMPLATE_DEVICES,
    .ndevices = sizeof(TEMPLATE_DEVICES) / sizeof(TEMPLATE_DEVICES[0]),
    .ops = {.battery_percent = template_battery_percent,
            .battery_mv = template_battery_mv,
            .reboot = template_reboot},
};

/* 4. The panel. present() sends one rectangle of the 8-bit framebuffer, expanded through the palette, and returns
 * only when the panel has it. Implement it with the panel's bus driver. */
static void template_present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *palette) {
    (void)ctx;
    (void)px;
    (void)stride;
    (void)r;
    (void)palette;
}

pal_display_ops_t template_display_ops(void) {
    pal_display_ops_t o = {.present = template_present, .backlight = NULL};
    return o;
}
