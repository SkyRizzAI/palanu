#include "pal/board.h"

#include <string.h>

#define LEGACY_MAX 3

static pal_board_desc_t B = {.name = "host", .chip = "none"};
static pal_device_t s_legacy[LEGACY_MAX]; /* devices made from ops.rgb and ops.beep (a board that declares none) */

static void legacy_led(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    (void)ctx, (void)pixel;
    if (B.ops.rgb) B.ops.rgb(r, g, b);
}
static void legacy_beep(void *ctx, int freq_hz, int ms) {
    (void)ctx;
    if (B.ops.beep) B.ops.beep(freq_hz, ms);
}

static int legacy_pct(void *ctx) {
    (void)ctx;
    return B.ops.battery_percent ? B.ops.battery_percent() : -1;
}
static int legacy_mv(void *ctx) {
    (void)ctx;
    return B.ops.battery_mv ? B.ops.battery_mv() : -1;
}

/* The summary bits that a device kind decides: they follow the list when there is one. */
#define DEVICE_CAPS (PAL_CAP_RGB_LED | PAL_CAP_BUZZER | PAL_CAP_BATTERY | PAL_CAP_SD | PAL_CAP_SENSORS | PAL_CAP_CAMERA | PAL_CAP_MIC | PAL_CAP_TOUCH)

static void caps_from_list(void) {
    B.caps &= ~DEVICE_CAPS;
    for (int i = 0; i < B.ndevices; i++) {
        const pal_device_t *d = &B.devices[i];
        if (d->kind == PAL_DEV_LED) B.caps |= PAL_CAP_RGB_LED;
        if (d->kind == PAL_DEV_BUZZER) B.caps |= PAL_CAP_BUZZER;
        if (d->kind == PAL_DEV_BATTERY) B.caps |= PAL_CAP_BATTERY;
        if (d->kind == PAL_DEV_STORAGE && d->storage.removable) B.caps |= PAL_CAP_SD;
        if (d->kind == PAL_DEV_SENSOR) B.caps |= PAL_CAP_SENSORS;
        if (d->kind == PAL_DEV_CAMERA) B.caps |= PAL_CAP_CAMERA;
        if (d->kind == PAL_DEV_MIC) B.caps |= PAL_CAP_MIC;
        if (d->kind == PAL_DEV_TOUCH) B.caps |= PAL_CAP_TOUCH;
        /* a radio device sets its bit; a board that declares no radio device keeps the bit it wrote (the radio manager) */
        if (d->kind == PAL_DEV_RADIO) B.caps |= d->radio.type == PAL_DEVRADIO_WIFI ? PAL_CAP_WIFI : PAL_CAP_BLE;
    }
}

void pal_board_set(const pal_board_desc_t *desc) {
    B = *desc;
    if (B.devices) {
        /* the list is the truth: the summary bits follow it */
        caps_from_list();
        return;
    }
    int n = 0;
    if ((B.caps & PAL_CAP_RGB_LED) && B.ops.rgb) {
        s_legacy[n] = (pal_device_t){.id = "led0", .name = "LED", .kind = PAL_DEV_LED, .led = {1, 3, legacy_led}};
        n++;
    }
    if ((B.caps & PAL_CAP_BUZZER) && B.ops.beep) {
        s_legacy[n] = (pal_device_t){.id = "buzzer0", .name = "Buzzer", .kind = PAL_DEV_BUZZER, .buzzer = {legacy_beep}};
        n++;
    }
    if ((B.caps & PAL_CAP_BATTERY) && (B.ops.battery_percent || B.ops.battery_mv)) {
        s_legacy[n] = (pal_device_t){.id = "battery0", .name = "Battery", .kind = PAL_DEV_BATTERY, .battery = {legacy_pct, legacy_mv}};
        n++;
    }
    B.devices = n ? s_legacy : NULL;
    B.ndevices = n;
}
const pal_board_desc_t *pal_board_info(void) { return &B; }
const pal_board_desc_t *pal_board(void) { return &B; }

bool pal_has(pal_cap_t cap) { return cap != 0 && (B.caps & cap) == cap; }

int pal_dev_count(pal_dev_kind_t kind) {
    int n = 0;
    for (int i = 0; i < B.ndevices; i++) n += B.devices[i].kind == kind;
    return n;
}
const pal_device_t *pal_dev_get(pal_dev_kind_t kind, int n) {
    if (n < 0) return NULL;
    for (int i = 0; i < B.ndevices; i++)
        if (B.devices[i].kind == kind && n-- == 0) return &B.devices[i];
    return NULL;
}
const pal_device_t *pal_dev_find(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < B.ndevices; i++)
        if (!strcmp(B.devices[i].id, id)) return &B.devices[i];
    return NULL;
}

void pal_led_set(const pal_device_t *d, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    if (!d || d->kind != PAL_DEV_LED || !d->led.set) return;
    if (d->led.colors < 3) { /* brightness only: the largest channel */
        uint8_t m = r > g ? r : g;
        r = g = b = m > b ? m : b;
    }
    if (pixel < 0) {
        for (int i = 0; i < d->led.pixels; i++) d->led.set(d->ctx, i, r, g, b);
    } else if (pixel < d->led.pixels) {
        d->led.set(d->ctx, pixel, r, g, b);
    }
}
void pal_led_all(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < B.ndevices; i++) pal_led_set(&B.devices[i], -1, r, g, b);
}
void pal_dev_beep(const pal_device_t *d, int freq_hz, int ms) {
    if (d && d->kind == PAL_DEV_BUZZER && d->buzzer.beep) d->buzzer.beep(d->ctx, freq_hz, ms);
}

void pal_beep(int ms) { pal_dev_beep(pal_dev_get(PAL_DEV_BUZZER, 0), 2700, ms); }
void pal_rgb(uint8_t r, uint8_t g, uint8_t b) { pal_led_all(r, g, b); }
int pal_dev_battery_percent(const pal_device_t *d) {
    return d && d->kind == PAL_DEV_BATTERY && d->battery.percent ? d->battery.percent(d->ctx) : -1;
}
int pal_dev_battery_mv(const pal_device_t *d) {
    return d && d->kind == PAL_DEV_BATTERY && d->battery.mv ? d->battery.mv(d->ctx) : -1;
}
int pal_battery_mv(void) { return pal_dev_battery_mv(pal_dev_get(PAL_DEV_BATTERY, 0)); }
int pal_battery(void) { return pal_dev_battery_percent(pal_dev_get(PAL_DEV_BATTERY, 0)); }

int pal_gpio_read(const pal_device_t *d) { return d && d->kind == PAL_DEV_GPIO && d->gpio.read ? d->gpio.read(d->ctx) : -1; }
int pal_gpio_write(const pal_device_t *d, int level) {
    return d && d->kind == PAL_DEV_GPIO && d->gpio.write ? d->gpio.write(d->ctx, level ? 1 : 0) : -1;
}
int pal_sensor_read(const pal_device_t *d, int32_t *milli) {
    return d && d->kind == PAL_DEV_SENSOR && d->sensor.read && milli ? d->sensor.read(d->ctx, milli) : -1;
}
int pal_bus_probe(const pal_device_t *d, int addr) {
    return d && d->kind == PAL_DEV_BUS && d->bus.probe ? d->bus.probe(d->ctx, addr) : -1;
}
int pal_bus_xfer(const pal_device_t *d, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn) {
    return d && d->kind == PAL_DEV_BUS && d->bus.xfer ? d->bus.xfer(d->ctx, addr, tx, txn, rx, rxn) : -1;
}
bool pal_dev_app_visible(const pal_device_t *d) {
    if (!d) return false;
    if (d->kind == PAL_DEV_GPIO) return d->gpio.app;
    if (d->kind == PAL_DEV_BUS) return d->bus.app;
    return d->kind == PAL_DEV_LED || d->kind == PAL_DEV_BUZZER || d->kind == PAL_DEV_BATTERY || d->kind == PAL_DEV_SENSOR;
}
const char *pal_dev_kind_name(pal_dev_kind_t k) {
    switch (k) {
    case PAL_DEV_LED: return "led";
    case PAL_DEV_BUZZER: return "buzzer";
    case PAL_DEV_BATTERY: return "battery";
    case PAL_DEV_STORAGE: return "storage";
    case PAL_DEV_SENSOR: return "sensor";
    case PAL_DEV_GPIO: return "gpio";
    case PAL_DEV_BUS: return "bus";
    case PAL_DEV_CAMERA: return "camera";
    case PAL_DEV_MIC: return "mic";
    case PAL_DEV_TOUCH: return "touch";
    case PAL_DEV_RADIO: return "radio";
    }
    return "?";
}
void pal_reboot(void) {
    if (B.ops.reboot) B.ops.reboot();
}
