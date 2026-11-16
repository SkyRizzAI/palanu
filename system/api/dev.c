/* The dev module of the palanu API (api/palanu/dev.h): the board's devices (pal/device.h) for apps. The glue checks the dev
 * permission before these run. An app's index is the position in the board's device list. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"
#include "pal/board.h"

static const pal_device_t *at(int32_t i) {
    const pal_board_desc_t *b = pal_board_info();
    /* a pin or bus the board did not mark for apps does not exist to an app */
    return i >= 0 && i < b->ndevices && pal_dev_app_visible(&b->devices[i]) ? &b->devices[i] : NULL;
}

int32_t psys_api_dev_total(api_ctx_t *c) {
    (void)c;
    return pal_board_info()->ndevices;
}

int32_t psys_api_dev_info(api_ctx_t *c, int32_t index, void *out, uint32_t len) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    char tmp[96];
    int n = snprintf(tmp, sizeof(tmp), "%s\t%s\t%s", d->id, pal_dev_kind_name(d->kind), d->name);
    if (d->kind == PAL_DEV_LED) n += snprintf(tmp + n, sizeof(tmp) - (size_t)n, "\t%d\t%d", d->led.pixels, d->led.colors);
    if (d->kind == PAL_DEV_SENSOR) n += snprintf(tmp + n, sizeof(tmp) - (size_t)n, "\t%s", d->sensor.unit ? d->sensor.unit : "");
    if (d->kind == PAL_DEV_GPIO)
        n += snprintf(tmp + n, sizeof(tmp) - (size_t)n, "\t%d\t%s", d->gpio.pin, d->gpio.write ? "rw" : "r");
    if (d->kind == PAL_DEV_BUS)
        n += snprintf(tmp + n, sizeof(tmp) - (size_t)n, "\t%s", d->bus.type == PAL_BUS_I2C ? "i2c" : d->bus.type == PAL_BUS_SPI ? "spi" : "uart");
    if (n < 0 || (uint32_t)n + 1 > len || n >= (int)sizeof(tmp)) return NEMA_ERR_INVAL;
    memcpy(out, tmp, (size_t)n + 1);
    return n;
}

int32_t psys_api_dev_lookup(api_ctx_t *c, const char *id) {
    (void)c;
    const pal_board_desc_t *b = pal_board_info();
    for (int i = 0; i < b->ndevices; i++)
        if (!strcmp(b->devices[i].id, id)) return i;
    return NEMA_ERR_NOENT;
}

int32_t psys_api_dev_set_led(api_ctx_t *c, int32_t index, int32_t pixel, uint32_t rgb) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_LED || pixel < -1 || pixel >= d->led.pixels) return NEMA_ERR_INVAL;
    pal_led_set(d, pixel, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
    return NEMA_OK;
}

int32_t psys_api_dev_play_tone(api_ctx_t *c, int32_t index, int32_t freq_hz, int32_t ms) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_BUZZER || freq_hz < 20 || freq_hz > 20000 || ms < 1 || ms > 5000) return NEMA_ERR_INVAL;
    pal_dev_beep(d, freq_hz, ms);
    return NEMA_OK;
}

int32_t psys_api_dev_gpio_read(api_ctx_t *c, int32_t index) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_GPIO || !d->gpio.read) return NEMA_ERR_INVAL;
    int v = pal_gpio_read(d);
    return v < 0 ? NEMA_ERR_IO : v;
}

int32_t psys_api_dev_gpio_write(api_ctx_t *c, int32_t index, int32_t level) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_GPIO || !d->gpio.write) return NEMA_ERR_INVAL;
    return pal_gpio_write(d, level) < 0 ? NEMA_ERR_IO : NEMA_OK;
}

int32_t psys_api_dev_sensor_read(api_ctx_t *c, int32_t index, void *out, uint32_t len) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_SENSOR) return NEMA_ERR_INVAL;
    int32_t v;
    if (len < 4) return NEMA_ERR_INVAL;
    if (pal_sensor_read(d, &v) < 0) return NEMA_ERR_IO;
    memcpy(out, &v, 4);
    return NEMA_OK;
}

int32_t psys_api_dev_read_battery(api_ctx_t *c, int32_t index) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_BATTERY) return NEMA_ERR_INVAL;
    return pal_dev_battery_percent(d);
}

int32_t psys_api_dev_i2c_probe(api_ctx_t *c, int32_t index, int32_t addr) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_BUS || d->bus.type != PAL_BUS_I2C || addr < 0x03 || addr > 0x77) return NEMA_ERR_INVAL;
    int v = pal_bus_probe(d, addr);
    return v < 0 ? NEMA_ERR_IO : v;
}

int32_t psys_api_dev_bus_xfer(api_ctx_t *c, int32_t index, int32_t addr, const void *tx, uint32_t txn, void *rx, uint32_t rxn) {
    (void)c;
    const pal_device_t *d = at(index);
    if (!d) return NEMA_ERR_NOENT;
    if (d->kind != PAL_DEV_BUS || !d->bus.xfer || txn > 256 || rxn > 256 || addr < 0) return NEMA_ERR_INVAL;
    return pal_bus_xfer(d, addr, tx, (int)txn, rx, (int)rxn) < 0 ? NEMA_ERR_IO : NEMA_OK;
}
