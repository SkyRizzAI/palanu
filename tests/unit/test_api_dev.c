/* The dev module (system/api/dev.c) on a board with several devices: indexes, ids, info text and the argument checks. The glue
 * checks the dev permission before these run (generated, covered by test_api_glue); here the implementations are called direct. */
#include <stdint.h>
#include <string.h>

#include "../../system/src/api_impl.h"
#include "nema/nema.h"
#include "pal/board.h"
#include "test.h"

static uint32_t g_last[4];
static int g_tone_hz, g_tone_ms;
static void led(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    (void)ctx;
    g_last[0] = (uint32_t)pixel, g_last[1] = r, g_last[2] = g, g_last[3] = b;
}
static void tone(void *ctx, int hz, int ms) {
    (void)ctx;
    g_tone_hz = hz, g_tone_ms = ms;
}
static const pal_device_t DEVS[] = {
    {.id = "led0", .name = "Left", .kind = PAL_DEV_LED, .led = {1, 3, led}},
    {.id = "strip", .name = "Strip", .kind = PAL_DEV_LED, .led = {4, 1, led}},
    {.id = "buzzer0", .name = "Buzzer", .kind = PAL_DEV_BUZZER, .buzzer = {tone}},
};
static int g_lv;
static int gr(void *c) { (void)c; return g_lv; }
static int gw(void *c, int l) { (void)c; return g_lv = l, 0; }
static int sr(void *c, int32_t *m) { (void)c; return *m = 21500, 0; }
static int bp(void *c) { (void)c; return 77; }
static int pr(void *c, int a) { (void)c; return a == 0x48; }
static int bx(void *c, int a, const uint8_t *t, int tn, uint8_t *r, int rn) { (void)c; (void)t; (void)tn; if (a != 3) return -1; for (int i = 0; i < rn; i++) r[i] = (uint8_t)(0xa0 + i); return 0; }
static const pal_device_t DEVS2[] = {
    {.id = "led0", .name = "Left", .kind = PAL_DEV_LED, .led = {1, 3, led}},
    {.id = "strip", .name = "Strip", .kind = PAL_DEV_LED, .led = {4, 1, led}},
    {.id = "buzzer0", .name = "Buzzer", .kind = PAL_DEV_BUZZER, .buzzer = {tone}},
    {.id = "secret", .name = "Secret", .kind = PAL_DEV_GPIO, .gpio = {2, false, gr, gw}},
    {.id = "g0", .name = "Pin 9", .kind = PAL_DEV_GPIO, .gpio = {9, true, gr, gw}},
    {.id = "g1", .name = "Pin 10", .kind = PAL_DEV_GPIO, .gpio = {10, true, gr, NULL}},
    {.id = "t0", .name = "Temp", .kind = PAL_DEV_SENSOR, .sensor = {"C", sr}},
    {.id = "bat", .name = "Battery", .kind = PAL_DEV_BATTERY, .battery = {.percent = bp}},
    {.id = "i2c0", .name = "I2C", .kind = PAL_DEV_BUS, .bus = {PAL_BUS_I2C, true, pr, bx}},
    {.id = "flash", .name = "Flash", .kind = PAL_DEV_STORAGE},
};
static const pal_board_desc_t B2 = {.name = "fake2", .chip = "fake", .caps = PAL_CAP_DISPLAY, .devices = DEVS2, .ndevices = 10};
static const pal_board_desc_t B = {.name = "fake", .chip = "fake", .caps = PAL_CAP_DISPLAY, .devices = DEVS, .ndevices = 3};

TEST(total_lookup_and_info) {
    pal_board_set(&B);
    CHECK_EQ(psys_api_dev_total(NULL), 3);
    CHECK_EQ(psys_api_dev_lookup(NULL, "strip"), 1);
    CHECK_EQ(psys_api_dev_lookup(NULL, "nope"), NEMA_ERR_NOENT);
    char out[64];
    CHECK(psys_api_dev_info(NULL, 1, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "strip\tled\tStrip\t4\t1") == 0);
    CHECK(psys_api_dev_info(NULL, 2, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "buzzer0\tbuzzer\tBuzzer") == 0);
    CHECK_EQ(psys_api_dev_info(NULL, 3, out, sizeof(out)), NEMA_ERR_NOENT);
    CHECK_EQ(psys_api_dev_info(NULL, -1, out, sizeof(out)), NEMA_ERR_NOENT);
    CHECK_EQ(psys_api_dev_info(NULL, 0, out, 4), NEMA_ERR_INVAL); /* does not fit */
}

TEST(set_led_checks_kind_and_pixel) {
    pal_board_set(&B);
    CHECK_EQ(psys_api_dev_set_led(NULL, 0, 0, 0x102030), NEMA_OK);
    CHECK_EQ(g_last[1], 0x10u);
    CHECK_EQ(g_last[3], 0x30u);
    CHECK_EQ(psys_api_dev_set_led(NULL, 1, -1, 0x0a1428), NEMA_OK); /* mono strip: the brightest channel on every pixel */
    CHECK_EQ(g_last[0], 3u);
    CHECK_EQ(g_last[2], 0x28u);
    CHECK_EQ(psys_api_dev_set_led(NULL, 1, 4, 1), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_dev_set_led(NULL, 1, -2, 1), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_dev_set_led(NULL, 2, 0, 1), NEMA_ERR_INVAL); /* a buzzer */
    CHECK_EQ(psys_api_dev_set_led(NULL, 9, 0, 1), NEMA_ERR_NOENT);
}

TEST(play_tone_checks_kind_and_range) {
    pal_board_set(&B);
    CHECK_EQ(psys_api_dev_play_tone(NULL, 2, 440, 50), NEMA_OK);
    CHECK_EQ(g_tone_hz, 440);
    CHECK_EQ(g_tone_ms, 50);
    CHECK_EQ(psys_api_dev_play_tone(NULL, 2, 10, 50), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_dev_play_tone(NULL, 2, 440, 6000), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_dev_play_tone(NULL, 0, 440, 50), NEMA_ERR_INVAL); /* an LED */
    CHECK_EQ(psys_api_dev_play_tone(NULL, 5, 440, 50), NEMA_ERR_NOENT);
}

TEST(pins_sensors_batteries_and_buses) {
    pal_board_set(&B2);
    CHECK_EQ(psys_api_dev_gpio_read(NULL, 3), NEMA_ERR_NOENT); /* a pin the board did not open to apps does not exist to one */
    char out[64];
    CHECK_EQ(psys_api_dev_info(NULL, 3, out, sizeof(out)), NEMA_ERR_NOENT);
    CHECK(psys_api_dev_info(NULL, 4, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "g0\tgpio\tPin 9\t9\trw") == 0);
    CHECK(psys_api_dev_info(NULL, 5, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "g1\tgpio\tPin 10\t10\tr") == 0);
    CHECK_EQ(psys_api_dev_gpio_write(NULL, 4, 5), NEMA_OK);
    CHECK_EQ(psys_api_dev_gpio_read(NULL, 4), 1);
    CHECK_EQ(psys_api_dev_gpio_write(NULL, 5, 1), NEMA_ERR_INVAL); /* input only */
    CHECK_EQ(psys_api_dev_gpio_read(NULL, 0), NEMA_ERR_INVAL);     /* an LED */
    int32_t v = 0;
    CHECK_EQ(psys_api_dev_sensor_read(NULL, 6, &v, 4), NEMA_OK);
    CHECK_EQ(v, 21500);
    CHECK_EQ(psys_api_dev_sensor_read(NULL, 6, &v, 2), NEMA_ERR_INVAL);
    CHECK(psys_api_dev_info(NULL, 6, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "t0\tsensor\tTemp\tC") == 0);
    CHECK_EQ(psys_api_dev_read_battery(NULL, 7), 77);
    CHECK_EQ(psys_api_dev_i2c_probe(NULL, 8, 0x48), 1);
    CHECK_EQ(psys_api_dev_i2c_probe(NULL, 8, 0x49), 0);
    CHECK_EQ(psys_api_dev_i2c_probe(NULL, 8, 0x01), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_dev_i2c_probe(NULL, 4, 0x48), NEMA_ERR_INVAL); /* not a bus */
    CHECK_EQ(psys_api_dev_gpio_read(NULL, 9), NEMA_ERR_NOENT);       /* storage is not for apps */
    uint8_t rx[2] = {0}, tx = 1;
    CHECK_EQ(psys_api_dev_bus_xfer(NULL, 8, 3, &tx, 1, rx, 2), NEMA_OK);
    CHECK_EQ(rx[1], 0xa1);
    CHECK_EQ(psys_api_dev_bus_xfer(NULL, 8, 4, &tx, 1, rx, 2), NEMA_ERR_IO);
    CHECK_EQ(psys_api_dev_bus_xfer(NULL, 8, 3, &tx, 300, rx, 2), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_dev_bus_xfer(NULL, 4, 3, &tx, 1, rx, 2), NEMA_ERR_INVAL); /* not a bus */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(total_lookup_and_info);
    RUN(set_led_checks_kind_and_pixel);
    RUN(play_tone_checks_kind_and_range);
    RUN(pins_sensors_batteries_and_buses);
    return TEST_DONE();
}
