/* Settings pages for the hardware (the reference camera, touch, sensors, secure element and LED screens). Each page has the reference's
 * section and rows; a row whose hardware the board lacks says so (No camera hardware, No sensors, Not present) or is N/A.
 * Nothing here reads a value the board does not report: the values come from pal_has() and the pal_* hooks. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/board.h"
#include "settings_int.h"

/* ---- Camera: Cameras, then the capture test when there is a camera ---- */

static pal_menu_row_t CAMERA_STORE[4] NEMA_EXT_BSS;

static void build_camera(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Cameras"};
    pal_menu_add(p, &r);
    if (!pal_has(PAL_CAP_CAMERA)) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No camera hardware"};
        pal_menu_add(p, &r);
        return;
    }
    /* the board has a camera, but no driver is wired yet: the capture test is not available */
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Camera", .text = settings_text_static, .ctx = (void *)"no driver yet"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Capture test", .disabled = true, .text = settings_text_static,
                         .ctx = (void *)"N/A"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_CAMERA = {.title = "Camera", .store = CAMERA_STORE, .cap = 4, .build = build_camera};

/* ---- Touch: the Touch Test, which PalanuOS does not ship ---- */

static pal_menu_row_t TOUCH_STORE[3] NEMA_EXT_BSS;

static void build_touch(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Touch"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Touch Test", .disabled = true, .text = settings_text_static,
                         .ctx = (void *)(pal_has(PAL_CAP_TOUCH) ? "no test app yet" : "N/A")};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_TOUCH = {.title = "Touch", .store = TOUCH_STORE, .cap = 3, .build = build_touch};

/* ---- Sensors: one section per sensor. No board driver reports a value yet, so a
 * board with sensors says so and shows no reading. ---- */

#define SENSORS_MAX 16
static pal_menu_row_t SENSORS_STORE[SENSORS_MAX + 2] NEMA_EXT_BSS;

/* One row per sensor the board declares; the value is read live. */
static void sensor_text(void *ctx, char *out, size_t cap) {
    const pal_device_t *d = ctx;
    int32_t m;
    if (pal_sensor_read(d, &m) < 0) snprintf(out, cap, "error");
    else {
        int64_t a = m < 0 ? -(int64_t)m : m;
        snprintf(out, cap, "%s%ld.%02ld %s", m < 0 ? "-" : "", (long)(a / 1000), (long)(a % 1000 / 10), d->sensor.unit ? d->sensor.unit : "");
    }
}

static void build_sensors(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Sensors"};
    pal_menu_add(p, &r);
    int n = pal_dev_count(PAL_DEV_SENSOR);
    if (n == 0) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No sensors"};
        pal_menu_add(p, &r);
    }
    for (int i = 0; i < n && i < SENSORS_MAX; i++) {
        const pal_device_t *d = pal_dev_get(PAL_DEV_SENSOR, i);
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = d->name, .text = sensor_text, .ctx = (void *)d};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_SENSORS = {.title = "Sensors", .store = SENSORS_STORE, .cap = SENSORS_MAX + 2, .build = build_sensors, .live = true};

/* ---- Secure Element: no board descriptor has one, so it is not present ---- */

static pal_menu_row_t SECURE_STORE[3] NEMA_EXT_BSS;

static void build_secure(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Secure Element"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Not present"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_SECURE = {.title = "Secure Element", .store = SECURE_STORE, .cap = 3, .build = build_secure};

/* ---- LEDs, from the board's device list (pal/device.h, ADR-0005): one LED is tested right
 * on this page; several get an "All LEDs" test and one page each, and a strip's pixels are tested one by one. ---- */

typedef struct {
    const pal_device_t *dev; /* NULL: every LED */
    int pixel;               /* -1: the whole device */
    uint8_t rgb[3];
} led_act_t;

static void led_run(void *ctx) {
    const led_act_t *a = (const led_act_t *)ctx;
    if (a->dev) pal_led_set(a->dev, a->pixel, a->rgb[0], a->rgb[1], a->rgb[2]);
    else pal_led_all(a->rgb[0], a->rgb[1], a->rgb[2]);
}

static const struct {
    const char *name;
    uint8_t rgb[3];
} LED_TESTS[] = {{"Red", {255, 0, 0}}, {"Green", {0, 255, 0}}, {"Blue", {0, 0, 255}}, {"White", {255, 255, 255}}, {"Off", {0, 0, 0}}};
#define LED_NTESTS 5
#define LED_PIXEL_ROWS 8

static led_act_t s_acts[LED_NTESTS + 1] NEMA_EXT_BSS;                       /* the root page's tests */
static led_act_t s_dev_acts[LED_NTESTS + LED_PIXEL_ROWS] NEMA_EXT_BSS;       /* the device page's tests */
static const pal_device_t *s_dev;                                /* the device the device page shows */
static char s_dev_title[24] NEMA_EXT_BSS;

/* Adds the five colour tests for dev (NULL: every LED) and pixel (-1: whole), using acts from index n. */
static int add_tests(pal_menu_page_t *p, led_act_t *acts, int n, const pal_device_t *dev, int pixel) {
    for (int i = 0; i < LED_NTESTS; i++, n++) {
        acts[n] = (led_act_t){.dev = dev, .pixel = pixel, .rgb = {LED_TESTS[i].rgb[0], LED_TESTS[i].rgb[1], LED_TESTS[i].rgb[2]}};
        pal_menu_row_t r = {.kind = PAL_MENU_ACTION, .label = LED_TESTS[i].name, .icon = "default", .ctx = &acts[n], .run = led_run};
        pal_menu_add(p, &r);
    }
    return n;
}

static const char *led_detail(const pal_device_t *d, char *buf, size_t cap) {
    snprintf(buf, cap, "%d pixel%s, %s", d->led.pixels, d->led.pixels == 1 ? "" : "s", d->led.colors >= 3 ? "RGB" : "mono");
    return buf;
}

static pal_menu_row_t LED_DEV_STORE[24] NEMA_EXT_BSS;

static void build_led_dev(pal_menu_page_t *p) {
    pal_menu_clear(p);
    const pal_device_t *d = s_dev;
    if (!d) return;
    static char info[32] NEMA_EXT_BSS;
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .inert = true, .label = led_detail(d, info, sizeof(info)), .text = settings_text_static, .ctx = (void *)d->id};
    pal_menu_add(p, &r);
    int n = add_tests(p, s_dev_acts, 0, d, -1);
    int px = d->led.pixels > 1 ? (d->led.pixels < LED_PIXEL_ROWS ? d->led.pixels : LED_PIXEL_ROWS) : 0;
    if (px) {
        r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Pixels"};
        pal_menu_add(p, &r);
    }
    static char names[LED_PIXEL_ROWS][12] NEMA_EXT_BSS;
    for (int i = 0; i < px; i++, n++) {
        snprintf(names[i], sizeof(names[i]), "Pixel %d", i + 1);
        s_dev_acts[n] = (led_act_t){.dev = d, .pixel = i, .rgb = {255, 255, 255}};
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = names[i], .icon = "default", .ctx = &s_dev_acts[n], .run = led_run};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_LED_DEV = {.title = s_dev_title, .store = LED_DEV_STORE, .cap = 24, .build = build_led_dev};

static pal_menu_page_t *led_open(void *ctx) {
    s_dev = (const pal_device_t *)ctx;
    snprintf(s_dev_title, sizeof(s_dev_title), "%s", s_dev->name);
    return &PG_LED_DEV;
}

static pal_menu_row_t LED_STORE[24] NEMA_EXT_BSS;

static void build_led(pal_menu_page_t *p) {
    pal_menu_clear(p);
    int count = pal_dev_count(PAL_DEV_LED);
    pal_menu_row_t r;
    if (count == 0) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No LEDs"};
        pal_menu_add(p, &r);
        return;
    }
    if (count == 1) { /* the one LED, tested here */
        const pal_device_t *d = pal_dev_get(PAL_DEV_LED, 0);
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = d->name, .text = settings_text_static, .ctx = (void *)(d->led.colors >= 3 ? "RGB" : "Mono")};
        pal_menu_add(p, &r);
        add_tests(p, s_acts, 0, d, -1);
        return;
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Test (all LEDs)"};
    pal_menu_add(p, &r);
    add_tests(p, s_acts, 0, NULL, -1);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Each LED"};
    pal_menu_add(p, &r);
    for (int i = 0; i < count && i < 12; i++) {
        const pal_device_t *d = pal_dev_get(PAL_DEV_LED, i);
        r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = d->name, .icon = "default", .ctx = (void *)d, .open = led_open};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_LED = {.title = "LEDs", .store = LED_STORE, .cap = 24, .build = build_led};
