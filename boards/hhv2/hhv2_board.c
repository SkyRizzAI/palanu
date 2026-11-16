/* The hhv2 board descriptor. Everything the system knows about this board is in this table: capabilities,
 * geometry, keyboard and hooks. The drivers are in hhv2_hw.c and hhv2_keyboard.c. */
#include "hhv2.h"

static void hhv2_led_set(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    (void)ctx, (void)pixel; /* one pixel */
    hhv2_rgb(r, g, b);
}
static int hhv2_dev_battery_percent(void *ctx) {
    (void)ctx;
    return hhv2_battery_percent();
}
static int hhv2_dev_battery_mv(void *ctx) {
    (void)ctx;
    return hhv2_battery_mv();
}
static void hhv2_buzzer_beep(void *ctx, int freq_hz, int ms) {
    (void)ctx;
    hhv2_beep(freq_hz, ms);
}

/* The devices: one RGB LED (a single WS2812) and the buzzer. A board with more LEDs lists more entries here. */
static const pal_device_t HHV2_DEVICES[] = {
    {.id = "led0", .name = "RGB LED", .kind = PAL_DEV_LED, .led = {.pixels = 1, .colors = 3, .set = hhv2_led_set}},
    {.id = "buzzer0", .name = "Buzzer", .kind = PAL_DEV_BUZZER, .buzzer = {.beep = hhv2_buzzer_beep}},
    {.id = "battery0", .name = "Battery", .kind = PAL_DEV_BATTERY,
     .battery = {.percent = hhv2_dev_battery_percent, .mv = hhv2_dev_battery_mv}},
    {.id = "flash0", .name = "Internal flash", .kind = PAL_DEV_STORAGE, .storage = {.removable = false}},
    {.id = "wifi0", .name = "Wi-Fi", .kind = PAL_DEV_RADIO, .radio = {PAL_DEVRADIO_WIFI}},
    {.id = "ble0", .name = "Bluetooth LE", .kind = PAL_DEV_RADIO, .radio = {PAL_DEVRADIO_BLE}},
    {.id = "sd0", .name = "microSD", .kind = PAL_DEV_STORAGE, .storage = {.removable = true}},
};
/* No GPIO, I2C or SPI device is declared: the board has not named a pin that is free for apps yet. Add .kind = PAL_DEV_GPIO
 * entries with .gpio.app = true for the header pins to open them; pins not listed do not exist to apps. */

const pal_board_desc_t HHV2_BOARD = {
    .name = "hhv2",
    .chip = "ESP32-S31",
    /* PSRAM: the tests/hw bring-up projects require esp_psram. SD: the card's chip select is on the pinout (PIN_SD_CS),
     * but no driver mounts it yet. */
    .caps = PAL_CAP_DISPLAY | PAL_CAP_KEYBOARD | PAL_CAP_PSRAM | PAL_CAP_WIFI | PAL_CAP_BLE,
    .display_w = HHV2_LCD_W,
    .display_h = HHV2_LCD_H,
    .display_format = PAL_PIXFMT_RGB565,
    .keyboard = &HHV2_KEYBOARD,
    .devices = HHV2_DEVICES,
    .ndevices = sizeof(HHV2_DEVICES) / sizeof(HHV2_DEVICES[0]),
    .ops = {.reboot = hhv2_reboot},
    .legend_keys = HHV2_LEGEND_KEYS,
    .ui_scale_q = 8, /* 2x: 160x120 logical, every logical pixel a 2x2 block of the 2.4" panel */
};
