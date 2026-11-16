/* The `dev` command: the board's devices (pal/device.h) from the terminal, so a board's LEDs and buzzers can be listed and
 * driven raw over UART, SSH or `palanu run` without an app. Whatever the board declares shows up; nothing here knows a board. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/board.h"
#include "pal/device.h"

static const char *kind_name(pal_dev_kind_t k) { return pal_dev_kind_name(k); }

static void list(void) {
    const pal_board_desc_t *b = pal_board_info();
    if (b->ndevices == 0) {
        pal_printf("no devices on %s\n", b->name);
        return;
    }
    pal_printf("%-10s %-7s %s\n", "ID", "KIND", "NAME");
    for (int i = 0; i < b->ndevices; i++) {
        const pal_device_t *d = &b->devices[i];
        pal_printf("%-10s %-7s %s", d->id, kind_name(d->kind), d->name);
        if (d->kind == PAL_DEV_LED) pal_printf("  (%d pixel%s, %s)", d->led.pixels, d->led.pixels == 1 ? "" : "s", d->led.colors >= 3 ? "RGB" : "mono");
        if (d->kind == PAL_DEV_SENSOR) pal_printf("  (%s)", d->sensor.unit ? d->sensor.unit : "?");
        if (d->kind == PAL_DEV_GPIO)
            pal_printf("  (pin %d, %s%s)", d->gpio.pin, d->gpio.write ? "in/out" : "in", d->gpio.app ? ", apps" : "");
        if (d->kind == PAL_DEV_BUS)
            pal_printf("  (%s%s)", d->bus.type == PAL_BUS_I2C ? "I2C" : d->bus.type == PAL_BUS_SPI ? "SPI" : "UART",
                       d->bus.app ? ", apps" : "");
        if (d->kind == PAL_DEV_RADIO) pal_printf("  (%s)", d->radio.type == PAL_DEVRADIO_WIFI ? "Wi-Fi" : "BLE");
        if (d->kind == PAL_DEV_STORAGE) pal_printf("  (%s)", d->storage.removable ? "card slot" : "flash");
        pal_printf("\n");
    }
}

static const struct {
    const char *name;
    uint8_t r, g, b;
} COLORS[] = {{"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255}, {"white", 255, 255, 255},
              {"yellow", 255, 200, 0}, {"cyan", 0, 255, 255}, {"magenta", 255, 0, 255}, {"off", 0, 0, 0}};

/* A colour from one argument: a name, or #rrggbb. */
static bool colour(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
    for (size_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]); i++)
        if (!strcmp(s, COLORS[i].name)) return *r = COLORS[i].r, *g = COLORS[i].g, *b = COLORS[i].b, true;
    /* a hex colour: #rrggbb, 0xrrggbb or rrggbb (the shell reads an unquoted # as a comment) */
    const char *h = s[0] == '#' ? s + 1 : (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? s + 2 : s;
    if (strlen(h) == 6) {
        char *end;
        unsigned long v = strtoul(h, &end, 16);
        if (*end) return false;
        return *r = (uint8_t)(v >> 16), *g = (uint8_t)(v >> 8), *b = (uint8_t)v, true;
    }
    return false;
}

static int led(int argc, char **argv) {
    if (argc < 4) goto usage;
    const pal_device_t *d = pal_dev_find(argv[2]);
    if (!d || d->kind != PAL_DEV_LED) {
        pal_eprintf("dev: no LED called %s (dev list)\n", argv[2]);
        return 1;
    }
    /* After the id: [PIXEL|all] then a colour, either one word (name or #rrggbb) or three numbers. */
    int i = 3, pixel = -1;
    if (!strcmp(argv[i], "all")) i++;
    else if (argc - i == 2 || argc - i == 4) pixel = atoi(argv[i++]);
    uint8_t r, g, b;
    int left = argc - i;
    if (left == 1 && colour(argv[i], &r, &g, &b)) {
    } else if (left == 3) {
        r = (uint8_t)atoi(argv[i]), g = (uint8_t)atoi(argv[i + 1]), b = (uint8_t)atoi(argv[i + 2]);
    } else goto usage;
    if (pixel >= d->led.pixels) {
        pal_eprintf("dev: %s has %d pixel%s\n", d->id, d->led.pixels, d->led.pixels == 1 ? "" : "s");
        return 1;
    }
    pal_led_set(d, pixel, r, g, b);
    return 0;
usage:
    pal_eprintf("usage: dev led ID [PIXEL|all] (red|green|blue|white|yellow|cyan|magenta|off|#rrggbb | 0xrrggbb | R G B)\n");
    return 1;
}

static int beep(int argc, char **argv) {
    const pal_device_t *d = argc >= 3 ? pal_dev_find(argv[2]) : pal_dev_get(PAL_DEV_BUZZER, 0);
    if (!d || d->kind != PAL_DEV_BUZZER) {
        pal_eprintf("dev: no buzzer%s%s (dev list)\n", argc >= 3 ? " called " : "", argc >= 3 ? argv[2] : "");
        return 1;
    }
    pal_dev_beep(d, argc >= 4 ? atoi(argv[3]) : 2700, argc >= 5 ? atoi(argv[4]) : 120);
    return 0;
}

static int gpio(int argc, char **argv) {
    const pal_device_t *d = argc >= 3 ? pal_dev_find(argv[2]) : NULL;
    if (!d || d->kind != PAL_DEV_GPIO) {
        pal_eprintf("usage: dev gpio ID [0|1]   (ids: dev list)\n");
        return 1;
    }
    if (argc >= 4) {
        if (pal_gpio_write(d, atoi(argv[3])) < 0) {
            pal_eprintf("dev: %s cannot be written\n", d->id);
            return 1;
        }
        return 0;
    }
    int v = pal_gpio_read(d);
    if (v < 0) {
        pal_eprintf("dev: %s cannot be read\n", d->id);
        return 1;
    }
    pal_printf("%d\n", v);
    return 0;
}

/* A sensor value (milli-units) as 21.500. */
static void print_milli(int32_t m) {
    pal_printf("%s%ld.%03ld", m < 0 ? "-" : "", (long)(m < 0 ? -(int64_t)m : m) / 1000, (long)(m < 0 ? -(int64_t)m : m) % 1000);
}

static int sensor(int argc, char **argv) {
    int shown = 0;
    for (int i = 0; i < pal_dev_count(PAL_DEV_SENSOR); i++) {
        const pal_device_t *d = pal_dev_get(PAL_DEV_SENSOR, i);
        if (argc >= 3 && strcmp(argv[2], d->id)) continue;
        int32_t m;
        pal_printf("%-10s ", d->id);
        if (pal_sensor_read(d, &m) < 0) pal_printf("error");
        else print_milli(m), pal_printf(" %s", d->sensor.unit ? d->sensor.unit : "");
        pal_printf("\n");
        shown++;
    }
    if (!shown) pal_eprintf("dev: no sensor%s%s\n", argc >= 3 ? " called " : "", argc >= 3 ? argv[2] : "");
    return shown ? 0 : 1;
}

static int battery(void) {
    const pal_device_t *d = pal_dev_get(PAL_DEV_BATTERY, 0);
    if (!d) return pal_eprintf("dev: no battery\n"), 1;
    int p = pal_dev_battery_percent(d), mv = pal_dev_battery_mv(d);
    pal_printf("%s: ", d->id);
    if (p >= 0) pal_printf("%d%%", p); else pal_printf("unknown");
    if (mv >= 0) pal_printf(" %d mV", mv);
    pal_printf("\n");
    return 0;
}

static int i2c(int argc, char **argv) {
    const pal_device_t *d = argc >= 4 ? pal_dev_find(argv[2]) : NULL;
    if (!d || d->kind != PAL_DEV_BUS || d->bus.type != PAL_BUS_I2C || strcmp(argv[3], "scan")) {
        pal_eprintf("usage: dev i2c ID scan\n");
        return 1;
    }
    int found = 0;
    for (int a = 0x08; a <= 0x77; a++)
        if (pal_bus_probe(d, a) == 1) pal_printf("0x%02x\n", a), found++;
    if (!found) pal_printf("nothing answers on %s\n", d->id);
    return 0;
}

/* dev bus ID ADDR [TXHEX [RXN]]: one transfer, e.g. `dev bus i2c0 0x48 00 2` writes 00 and reads two bytes. */
static int bus(int argc, char **argv) {
    const pal_device_t *d = argc >= 4 ? pal_dev_find(argv[2]) : NULL;
    if (!d || d->kind != PAL_DEV_BUS) {
        pal_eprintf("usage: dev bus ID ADDR [TXHEX [RXN]]\n");
        return 1;
    }
    uint8_t tx[64], rx[64];
    int txn = 0, rxn = argc >= 6 ? atoi(argv[5]) : 0;
    if (argc >= 5) {
        const char *h = argv[4];
        size_t n = strlen(h);
        if (n % 2 || n / 2 > sizeof(tx) || rxn < 0 || rxn > (int)sizeof(rx)) return pal_eprintf("dev: bad hex or length\n"), 1;
        for (size_t i = 0; i < n; i += 2) {
            char b[3] = {h[i], h[i + 1], 0}, *end;
            tx[txn++] = (uint8_t)strtoul(b, &end, 16);
            if (*end) return pal_eprintf("dev: bad hex\n"), 1;
        }
    }
    if (pal_bus_xfer(d, (int)strtol(argv[3], NULL, 0), tx, txn, rx, rxn) < 0) return pal_eprintf("dev: transfer failed\n"), 1;
    for (int i = 0; i < rxn; i++) pal_printf("%02x", rx[i]);
    if (rxn) pal_printf("\n");
    return 0;
}

static int cmd_dev(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "list")) return list(), 0;
    if (!strcmp(argv[1], "led")) return led(argc, argv);
    if (!strcmp(argv[1], "beep")) return beep(argc, argv);
    if (!strcmp(argv[1], "gpio")) return gpio(argc, argv);
    if (!strcmp(argv[1], "sensor")) return sensor(argc, argv);
    if (!strcmp(argv[1], "battery")) return battery();
    if (!strcmp(argv[1], "i2c")) return i2c(argc, argv);
    if (!strcmp(argv[1], "bus")) return bus(argc, argv);
    pal_eprintf("usage: dev [list]\n       dev led ID [PIXEL|all] COLOUR\n       dev beep [ID [HZ [MS]]]\n"
                "       dev gpio ID [0|1]\n       dev sensor [ID]\n       dev battery\n       dev i2c ID scan\n       dev bus ID ADDR [TXHEX [RXN]]\n");
    return 1;
}

static const nema_app_t DEV_APPS[] = {{"dev", "list and drive the board's devices", cmd_dev, 0, 0}};

void pal_apps_register_dev(void);
void pal_apps_register_dev(void) { nema_app_register(&DEV_APPS[0]); }
