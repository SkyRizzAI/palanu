/* The board descriptor (system/src/board.c): the default before a board is set, capability checks for set and
 * clear bits, and the helpers that gate battery, buzzer and LED on their capability. The hooks are counting
 * fakes; no board driver is linked. */
#include <stdint.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/board.h"
#include "test.h"

static int g_beeps, g_rgb_calls, g_battery_calls;

static void fake_beep(int freq_hz, int ms) {
    (void)freq_hz;
    (void)ms;
    g_beeps++;
}
static void fake_rgb(uint8_t r, uint8_t g, uint8_t b) {
    (void)r, (void)g, (void)b;
    g_rgb_calls++;
}
static int fake_battery_mv(void) {
    g_battery_calls++;
    return 3900;
}
static int fake_battery_pct(void) {
    g_battery_calls++;
    return 70;
}
static void fake_reboot(void) {}

static const pal_keyboard_t FAKE_KEYBOARD = {1, 1, NULL};

/* A board with a display, a keyboard, a battery, a buzzer and an LED. */
static const pal_board_desc_t FULL = {
    .name = "fake-full",
    .chip = "fake-chip",
    .caps = PAL_CAP_DISPLAY | PAL_CAP_KEYBOARD | PAL_CAP_BATTERY | PAL_CAP_BUZZER | PAL_CAP_RGB_LED,
    .display_w = 320,
    .display_h = 240,
    .display_format = PAL_PIXFMT_RGB565,
    .keyboard = &FAKE_KEYBOARD,
    .ops = {.beep = fake_beep,
            .rgb = fake_rgb,
            .battery_percent = fake_battery_pct,
            .battery_mv = fake_battery_mv,
            .reboot = fake_reboot},
};

/* A board that has the hooks but not the capabilities: the hooks must not run. */
static const pal_board_desc_t BARE = {
    .name = "fake-bare",
    .chip = "fake-chip",
    .caps = PAL_CAP_DISPLAY,
    .ops = {.beep = fake_beep,
            .rgb = fake_rgb,
            .battery_percent = fake_battery_pct,
            .battery_mv = fake_battery_mv,
            .reboot = fake_reboot},
};

/* Runs before any pal_board_set: the system has no board yet and must claim nothing. */
TEST(default_board_has_no_capabilities) {
    CHECK(!pal_has(PAL_CAP_DISPLAY));
    CHECK(!pal_has(PAL_CAP_BATTERY));
    CHECK_EQ(pal_battery(), -1);
    CHECK_EQ(pal_battery_mv(), -1);
    CHECK(strcmp(pal_board_info()->name, "host") == 0);
}

TEST(descriptor_lookup) {
    pal_board_set(&FULL);
    const pal_board_desc_t *d = pal_board_info();
    CHECK(strcmp(d->name, "fake-full") == 0);
    CHECK(strcmp(d->chip, "fake-chip") == 0);
    CHECK(d == pal_board());
    CHECK_EQ(d->display_w, 320);
    CHECK_EQ(d->display_h, 240);
    CHECK(d->display_format == PAL_PIXFMT_RGB565);
    CHECK(d->keyboard == &FAKE_KEYBOARD);
}

TEST(has_set_and_clear_bits) {
    pal_board_set(&FULL);
    CHECK(pal_has(PAL_CAP_DISPLAY));
    CHECK(pal_has(PAL_CAP_BATTERY));
    CHECK(pal_has(PAL_CAP_DISPLAY | PAL_CAP_KEYBOARD));
    CHECK(!pal_has(PAL_CAP_WIFI));
    CHECK(!pal_has(PAL_CAP_SD));
    CHECK(!pal_has(PAL_CAP_BATTERY | PAL_CAP_WIFI)); /* every bit must be set */
    CHECK(!pal_has(0));
}

TEST(battery_needs_capability) {
    int calls = g_battery_calls;
    pal_board_set(&BARE);
    CHECK_EQ(pal_battery(), -1);
    CHECK_EQ(pal_battery_mv(), -1);
    CHECK_EQ(g_battery_calls, calls);
    pal_board_set(&FULL);
    CHECK_EQ(pal_battery_mv(), 3900);
    CHECK_EQ(pal_battery(), 70);
}

TEST(beep_and_rgb_need_capability) {
    int beeps = g_beeps, rgb = g_rgb_calls;
    pal_board_set(&BARE);
    pal_beep(50);
    pal_rgb(1, 2, 3);
    CHECK_EQ(g_beeps, beeps);
    CHECK_EQ(g_rgb_calls, rgb);
    pal_board_set(&FULL);
    pal_beep(50);
    pal_rgb(1, 2, 3);
    CHECK_EQ(g_beeps, beeps + 1);
    CHECK_EQ(g_rgb_calls, rgb + 1);
}

/* ---- the device list (ADR-0005): five LEDs, a strip, a mono LED and two buzzers ---- */

static uint8_t g_px[8][8][3]; /* [device][pixel] = the last colour */
static int g_tones, g_last_hz;
static void dev_led(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    int i = (int)(intptr_t)ctx;
    g_px[i][pixel][0] = r, g_px[i][pixel][1] = g, g_px[i][pixel][2] = b;
}
static void dev_tone(void *ctx, int hz, int ms) {
    (void)ctx, (void)ms;
    g_tones++, g_last_hz = hz;
}

static const pal_device_t MULTI_DEVS[] = {
    {.id = "led0", .name = "LED 1", .kind = PAL_DEV_LED, .ctx = (void *)0, .led = {1, 3, dev_led}},
    {.id = "led1", .name = "LED 2", .kind = PAL_DEV_LED, .ctx = (void *)1, .led = {1, 3, dev_led}},
    {.id = "strip", .name = "Strip", .kind = PAL_DEV_LED, .ctx = (void *)2, .led = {4, 3, dev_led}},
    {.id = "mono", .name = "Mono", .kind = PAL_DEV_LED, .ctx = (void *)3, .led = {1, 1, dev_led}},
    {.id = "buzzer0", .name = "Buzzer", .kind = PAL_DEV_BUZZER, .buzzer = {dev_tone}},
    {.id = "buzzer1", .name = "Buzzer 2", .kind = PAL_DEV_BUZZER, .buzzer = {dev_tone}},
};
static const pal_board_desc_t MULTI = {.name = "fake-multi", .chip = "fake-chip", .caps = PAL_CAP_DISPLAY,
                                       .devices = MULTI_DEVS, .ndevices = sizeof(MULTI_DEVS) / sizeof(MULTI_DEVS[0])};

TEST(devices_are_found_by_kind_and_id) {
    pal_board_set(&MULTI);
    CHECK_EQ(pal_dev_count(PAL_DEV_LED), 4);
    CHECK_EQ(pal_dev_count(PAL_DEV_BUZZER), 2);
    CHECK(strcmp(pal_dev_get(PAL_DEV_LED, 2)->id, "strip") == 0);
    CHECK(strcmp(pal_dev_get(PAL_DEV_BUZZER, 1)->id, "buzzer1") == 0);
    CHECK(pal_dev_get(PAL_DEV_LED, 4) == NULL);
    CHECK(pal_dev_get(PAL_DEV_LED, -1) == NULL);
    CHECK(pal_dev_find("mono") == pal_dev_get(PAL_DEV_LED, 3));
    CHECK(pal_dev_find("nope") == NULL);
    CHECK(pal_has(PAL_CAP_RGB_LED) && pal_has(PAL_CAP_BUZZER)); /* the summary bits follow the list */
}

TEST(led_set_drives_one_device_one_pixel_or_all) {
    memset(g_px, 0, sizeof(g_px));
    pal_board_set(&MULTI);
    const pal_device_t *strip = pal_dev_find("strip");
    pal_led_set(strip, 2, 10, 20, 30);
    CHECK_EQ(g_px[2][2][1], 20);
    CHECK_EQ(g_px[2][1][1], 0);
    pal_led_set(strip, -1, 1, 2, 3);
    for (int i = 0; i < 4; i++) CHECK_EQ(g_px[2][i][2], 3);
    pal_led_set(strip, 4, 9, 9, 9); /* out of range: nothing */
    CHECK_EQ(g_px[0][0][0], 0);
    pal_led_set(pal_dev_find("buzzer0"), -1, 9, 9, 9); /* not an LED: nothing */
    CHECK_EQ(g_px[0][0][0], 0);
}

TEST(mono_led_gets_the_brightest_channel) {
    memset(g_px, 0, sizeof(g_px));
    pal_board_set(&MULTI);
    pal_led_set(pal_dev_find("mono"), 0, 10, 200, 30);
    CHECK_EQ(g_px[3][0][0], 200);
    CHECK_EQ(g_px[3][0][1], 200);
    CHECK_EQ(g_px[3][0][2], 200);
}

TEST(rgb_helper_reaches_every_led_and_beep_the_first_buzzer) {
    memset(g_px, 0, sizeof(g_px));
    g_tones = 0;
    pal_board_set(&MULTI);
    pal_rgb(5, 6, 7);
    CHECK_EQ(g_px[0][0][0], 5);
    CHECK_EQ(g_px[1][0][1], 6);
    for (int i = 0; i < 4; i++) CHECK_EQ(g_px[2][i][2], 7);
    pal_beep(30);
    CHECK_EQ(g_tones, 1); /* only the first buzzer */
    pal_dev_beep(pal_dev_find("buzzer1"), 440, 10);
    CHECK_EQ(g_tones, 2);
    CHECK_EQ(g_last_hz, 440);
}

TEST(legacy_hooks_become_one_led_and_one_buzzer) {
    pal_board_set(&FULL);
    CHECK_EQ(pal_dev_count(PAL_DEV_LED), 1);
    CHECK_EQ(pal_dev_count(PAL_DEV_BUZZER), 1);
    CHECK(strcmp(pal_dev_get(PAL_DEV_LED, 0)->id, "led0") == 0);
    int rgb = g_rgb_calls;
    pal_led_set(pal_dev_find("led0"), 0, 1, 2, 3);
    CHECK_EQ(g_rgb_calls, rgb + 1);
    pal_board_set(&BARE); /* hooks without the capability are not devices */
    CHECK_EQ(pal_dev_count(PAL_DEV_LED), 0);
    CHECK_EQ(pal_dev_count(PAL_DEV_BUZZER), 0);
}

static int g_level[2], g_bat = 55;
static int t_gread(void *ctx) { return g_level[(intptr_t)ctx]; }
static int t_gwrite(void *ctx, int l) { return g_level[(intptr_t)ctx] = l, 0; }
static int t_sens(void *ctx, int32_t *m) { return *m = -2500 + (int)(intptr_t)ctx, 0; }
static int t_bat(void *ctx) { (void)ctx; return g_bat; }
static int t_probe(void *ctx, int a) { (void)ctx; return a == 0x48; }
static const pal_device_t ALLK[] = {
    {.id = "bat", .name = "Battery", .kind = PAL_DEV_BATTERY, .battery = {.percent = t_bat}},
    {.id = "flash", .name = "Flash", .kind = PAL_DEV_STORAGE, .storage = {.removable = false}},
    {.id = "t0", .name = "Temp", .kind = PAL_DEV_SENSOR, .ctx = (void *)1, .sensor = {"C", t_sens}},
    {.id = "g0", .name = "Pin 4", .kind = PAL_DEV_GPIO, .ctx = (void *)0, .gpio = {4, true, t_gread, t_gwrite}},
    {.id = "g1", .name = "Pin 5", .kind = PAL_DEV_GPIO, .ctx = (void *)1, .gpio = {5, false, t_gread, NULL}},
    {.id = "i2c0", .name = "I2C", .kind = PAL_DEV_BUS, .bus = {PAL_BUS_I2C, true, t_probe}},
};
static const pal_board_desc_t ALLB = {.name = "fake-all", .chip = "x", .caps = PAL_CAP_DISPLAY | PAL_CAP_SD | PAL_CAP_BATTERY,
                                      .devices = ALLK, .ndevices = sizeof(ALLK) / sizeof(ALLK[0])};

TEST(summary_bits_follow_every_kind_of_device) {
    pal_board_set(&ALLB);
    CHECK(pal_has(PAL_CAP_BATTERY));
    CHECK(pal_has(PAL_CAP_SENSORS));
    CHECK(!pal_has(PAL_CAP_SD)); /* the declared storage is soldered flash, not a card slot: the stale bit is cleared */
    CHECK(pal_has(PAL_CAP_DISPLAY));
    CHECK_EQ(pal_battery(), 55);
    CHECK_EQ(pal_battery_mv(), -1);
}

TEST(gpio_sensor_and_bus_go_through_the_board_hooks) {
    pal_board_set(&ALLB);
    CHECK_EQ(pal_dev_count(PAL_DEV_GPIO), 2);
    CHECK_EQ(pal_gpio_write(pal_dev_find("g0"), 7), 0); /* any non-zero is high */
    CHECK_EQ(pal_gpio_read(pal_dev_find("g0")), 1);
    CHECK(pal_gpio_write(pal_dev_find("g1"), 1) < 0); /* input only */
    CHECK(pal_gpio_read(pal_dev_find("t0")) < 0);      /* wrong kind */
    int32_t m = 0;
    CHECK_EQ(pal_sensor_read(pal_dev_find("t0"), &m), 0);
    CHECK_EQ(m, -2499);
    CHECK_EQ(pal_bus_probe(pal_dev_find("i2c0"), 0x48), 1);
    CHECK_EQ(pal_bus_probe(pal_dev_find("i2c0"), 0x49), 0);
}

TEST(only_pins_the_board_opened_are_visible_to_apps) {
    pal_board_set(&ALLB);
    CHECK(pal_dev_app_visible(pal_dev_find("g0")));
    CHECK(!pal_dev_app_visible(pal_dev_find("g1")));
    CHECK(pal_dev_app_visible(pal_dev_find("i2c0")));
    CHECK(pal_dev_app_visible(pal_dev_find("t0")));
    CHECK(!pal_dev_app_visible(pal_dev_find("flash")));
}

static int t_xfer(void *ctx, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn) {
    (void)ctx;
    if (addr != 5) return -1;
    for (int i = 0; i < rxn; i++) rx[i] = (uint8_t)((txn ? tx[0] : 0) + i);
    return 0;
}
static const pal_device_t XK[] = {
    {.id = "spi0", .name = "SPI", .kind = PAL_DEV_BUS, .bus = {.type = PAL_BUS_SPI, .app = true, .xfer = t_xfer}},
    {.id = "cam", .name = "Camera", .kind = PAL_DEV_CAMERA},
    {.id = "mic", .name = "Mic", .kind = PAL_DEV_MIC},
    {.id = "tp", .name = "Touch", .kind = PAL_DEV_TOUCH},
    {.id = "ble0", .name = "BLE", .kind = PAL_DEV_RADIO, .radio = {PAL_DEVRADIO_BLE}},
};
static const pal_board_desc_t XB = {.name = "fake-x", .chip = "x", .caps = PAL_CAP_DISPLAY | PAL_CAP_WIFI, .devices = XK, .ndevices = 5};

TEST(bus_transfer_and_info_only_kinds) {
    pal_board_set(&XB);
    uint8_t rx[3] = {0};
    uint8_t tx = 0x10;
    CHECK_EQ(pal_bus_xfer(pal_dev_find("spi0"), 5, &tx, 1, rx, 3), 0);
    CHECK_EQ(rx[2], 0x12);
    CHECK(pal_bus_xfer(pal_dev_find("spi0"), 6, &tx, 1, rx, 3) < 0);
    CHECK(pal_bus_xfer(pal_dev_find("cam"), 5, &tx, 1, rx, 3) < 0);
    CHECK(pal_has(PAL_CAP_CAMERA) && pal_has(PAL_CAP_MIC) && pal_has(PAL_CAP_TOUCH));
    CHECK(pal_has(PAL_CAP_BLE));
    CHECK(pal_has(PAL_CAP_WIFI)); /* a radio the list does not mention keeps the bit the board wrote */
    CHECK(!pal_dev_app_visible(pal_dev_find("cam")));
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(default_board_has_no_capabilities);
    RUN(descriptor_lookup);
    RUN(has_set_and_clear_bits);
    RUN(battery_needs_capability);
    RUN(beep_and_rgb_need_capability);
    RUN(devices_are_found_by_kind_and_id);
    RUN(led_set_drives_one_device_one_pixel_or_all);
    RUN(mono_led_gets_the_brightest_channel);
    RUN(rgb_helper_reaches_every_led_and_beep_the_first_buzzer);
    RUN(legacy_hooks_become_one_led_and_one_buzzer);
    RUN(summary_bits_follow_every_kind_of_device);
    RUN(gpio_sensor_and_bus_go_through_the_board_hooks);
    RUN(only_pins_the_board_opened_are_visible_to_apps);
    RUN(bus_transfer_and_info_only_kinds);
    return TEST_DONE();
}
