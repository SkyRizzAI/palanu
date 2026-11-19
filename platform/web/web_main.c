/* The browser simulator entry (Emscripten). The page loads this module, calls pal_web_start, then drives it with
 * pal_web_key / pal_web_type and reads the screen from pal_web_frame. The board is the simulator's: a window and the hhv2
 * keyboard. Everything else (Wi-Fi networks, devices, battery) is configured from the page through the pal_web_* calls. */
#include <emscripten.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtin.h"
#include "sim_hw.h"
#include "nema/nema.h"
#include "pal/desktops.h"
#include "pal/board.h"
#include "pal/input.h"
#include "pal/svc.h"
#include "pal/wifi.h"
#include "pal/link.h"
#include "pal/radio.h"

#define MAX_W 320 /* the system draws at most 320x240 (KUI_MAX_W, KUI_MAX_H) */
#define MAX_H 240

static int W = 320, H = 240; /* the simulated panel: --screen WxH at start */

extern const pal_keyboard_t HHV2_KEYBOARD;
extern const char *const HHV2_LEGEND_KEYS[];

static int battery_percent(void) { return sim_hw_battery(); }

static pal_board_desc_t BOARD = {
    .name = "web",
    .chip = "wasm",
    .caps = PAL_CAP_DISPLAY | PAL_CAP_KEYBOARD | PAL_CAP_BATTERY,
    .display_w = 320,
    .display_h = 240,
    .display_format = PAL_PIXFMT_RGB565,
    .keyboard = &HHV2_KEYBOARD,
    .ops = {.battery_percent = battery_percent},
    .legend_keys = HHV2_LEGEND_KEYS,
    .ui_scale_q = 8,
};

static uint8_t s_rgba[MAX_W * MAX_H * 4];
static volatile int s_frame;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    (void)ctx;
    pthread_mutex_lock(&s_lock);
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint16_t v = pal[px[y * stride + x]];
            v = (uint16_t)((v >> 8) | (v << 8)); /* the palette is byte-swapped for SPI */
            uint8_t *o = &s_rgba[(y * W + x) * 4];
            o[0] = (uint8_t)((((v >> 11) & 31) * 255 + 15) / 31);
            o[1] = (uint8_t)((((v >> 5) & 63) * 255 + 31) / 63);
            o[2] = (uint8_t)(((v & 31) * 255 + 15) / 31);
            o[3] = 255;
        }
    s_frame++;
    pthread_mutex_unlock(&s_lock);
}

static void present565(void *ctx, const uint16_t *px, int stride, pal_rect_t r) {
    (void)ctx;
    pthread_mutex_lock(&s_lock);
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint16_t v = px[y * stride + x];
            uint8_t *o = &s_rgba[(y * W + x) * 4];
            o[0] = (uint8_t)((((v >> 11) & 31) * 255 + 15) / 31);
            o[1] = (uint8_t)((((v >> 5) & 63) * 255 + 31) / 63);
            o[2] = (uint8_t)(((v & 31) * 255 + 15) / 31);
            o[3] = 255;
        }
    s_frame++;
    pthread_mutex_unlock(&s_lock);
}

static void services(void) {
    psys_svc_builtin_t b;
    memset(&b, 0, sizeof(b));
    b.id = "input";
    b.name = "Keyboard input";
    b.restart = PSYS_SVC_RESTART_NEVER;
    b.autostart = true;
    b.core = true;
    psys_svc_register_builtin(&b);
}

static pal_status_t status(void) {
    pal_status_t st = {0, 0, sim_hw_battery() >= 0 ? sim_hw_battery() : pal_battery(), sim_hw_wifi_connected() ? 3 : 0, false, false, false};
    return st;
}

/* --desktops LIST: the sessions to ship, in order (classic,athena by default; "none" for the console only), as a target's list. */
static const pal_desktop_env_t *s_desktops[4];
static const pal_desktop_env_t *const *desktops_arg(int argc, char **argv) {
    const char *list = "classic,athena";
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--desktops")) list = argv[i + 1];
    const pal_desktop_env_t *const all[] = {&PAL_DESKTOP_CLASSIC, &PAL_DESKTOP_ATHENA};
    int n = 0;
    for (const char *p = list; *p && n < 3;) {
        size_t len = strcspn(p, ",");
        for (size_t k = 0; k < sizeof(all) / sizeof(all[0]); k++)
            if (strlen(all[k]->id) == len && !strncmp(all[k]->id, p, len)) s_desktops[n++] = all[k];
        p += len + (p[len] == ',');
    }
    s_desktops[n] = NULL;
    return s_desktops;
}

static int arg_int(int argc, char **argv, const char *flag, int def) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], flag)) return atoi(argv[i + 1]);
    return def;
}

static bool has_arg(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], flag)) return true;
    return false;
}

/* ---- the virtual cable: Web Forge reaches the simulator's Palanu Link as it reaches a board on USB ----
 * Two single-producer, single-consumer byte rings, no locks: the page writes rx and reads tx from the browser thread; the
 * "vlink" task reads rx and feeds the link, and the link's transmit (one at a time, under its transmit lock) writes tx. The
 * link's transport is the cable, so it is trusted like a USB link (link.uart.trust) and needs no pairing. */
#define VRING 65536u /* a power of two */
typedef struct {
    uint8_t b[VRING];
    atomic_uint head; /* the producer's */
    atomic_uint tail; /* the consumer's */
} vring_t;
static vring_t s_vrx, s_vtx;
static atomic_int s_vreset;

static size_t vring_put(vring_t *r, const uint8_t *d, size_t n) {
    unsigned h = atomic_load(&r->head), t = atomic_load(&r->tail);
    size_t room = VRING - (h - t), k = n < room ? n : room;
    for (size_t i = 0; i < k; i++) r->b[(h + i) & (VRING - 1)] = d[i];
    atomic_store(&r->head, h + (unsigned)k);
    return k;
}

static size_t vring_get(vring_t *r, uint8_t *d, size_t cap) {
    unsigned h = atomic_load(&r->head), t = atomic_load(&r->tail);
    size_t have = h - t, k = cap < have ? cap : have;
    for (size_t i = 0; i < k; i++) d[i] = r->b[(t + i) & (VRING - 1)];
    atomic_store(&r->tail, t + (unsigned)k);
    return k;
}

static int vlink_tx(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx;
    for (int waited = 0; n;) { /* the page drains tx every frame; a page that stopped reading drops the frame after 5 s */
        size_t k = vring_put(&s_vtx, d, n);
        d += k, n -= k;
        if (n && (waited += 2) > 5000) return -1;
        if (n) nema_sleep_ms(2);
    }
    return 0;
}

static void vlink_task(void *arg) {
    (void)arg;
    pal_link_io_t io = {.tx = vlink_tx, .transport = PAL_LINK_UART};
    pal_link_t *l = NULL;
    uint8_t b[512];
    for (;;) {
        if (atomic_exchange(&s_vreset, 0)) { /* a new connection from the page: a fresh link, nothing left of the last one */
            if (l) pal_link_destroy(l);
            l = NULL;
            atomic_store(&s_vrx.tail, atomic_load(&s_vrx.head));
        }
        size_t n = vring_get(&s_vrx, b, sizeof(b));
        if (!n) {
            nema_sleep_ms(4);
            continue;
        }
        if (!l) l = pal_link_create(&io);
        if (l) pal_link_feed(l, b, n);
    }
}

static void vlink_start(void) {
    nema_task_desc_t td = {.name = "vlink", .fn = vlink_task, .stack = 8192, .prio = 3, .core = -1,
                           .role = NEMA_TASK_SYSTEM, .owner = nema_owner_register("link")};
    if (nema_task_create(&td) != NEMA_OK) fprintf(stderr, "palanu-web: the virtual cable did not start\n");
}

/* The page connects: the link starts over (the page drains tx itself before it calls this). */
EMSCRIPTEN_KEEPALIVE void pal_web_link_reset(void) { atomic_store(&s_vreset, 1); }
/* Bytes from the page to the device; returns how many fit (the rest is the page's to retry). */
EMSCRIPTEN_KEEPALIVE int pal_web_link_write(const uint8_t *d, int n) { return n > 0 ? (int)vring_put(&s_vrx, d, (size_t)n) : 0; }
/* Bytes from the device to the page, at most cap. */
EMSCRIPTEN_KEEPALIVE int pal_web_link_read(uint8_t *out, int cap) { return cap > 0 ? (int)vring_get(&s_vtx, out, (size_t)cap) : 0; }

/* The page calls Module.callMain(args): --leds N --sensors N --gpios N --i2c --battery PCT --log. Boot runs on the worker thread
 * that main() lives on (PROXY_TO_PTHREAD). The simulated hardware is the native simulator's (platform/host/sim_hw.c). */
int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) {
        int w, h;
        if (!strcmp(argv[i], "--screen") && sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w >= 64 && w <= MAX_W && h >= 48 && h <= MAX_H) W = w, H = h;
    }
    BOARD.display_w = (uint16_t)W, BOARD.display_h = (uint16_t)H;
    sim_hw_set_battery(arg_int(argc, argv, "--battery", 80));
    sim_hw_cfg_t hw = {.leds = arg_int(argc, argv, "--leds", 0), .sensors = arg_int(argc, argv, "--sensors", 0),
                       .gpios = arg_int(argc, argv, "--gpios", 0), .i2c = has_arg(argc, argv, "--i2c"), .radios = true,
                       .log = has_arg(argc, argv, "--led-log")};
    sim_hw_init(&BOARD, &hw);
    pal_radio_set_ops(sim_hw_radio_ops());
    pal_wifi_set_ops(sim_hw_wifi_ops());
    pal_boot_cfg_t cfg = {
        .board = &BOARD,
        .storage_root = "/data",
        .display = {.present = present, .present_rgb565 = present565},
        .console_log = true,
        .status = status,
        .platform_services = services,
        .desktops = desktops_arg(argc, argv),
    };
    pal_boot(&cfg);
    vlink_start();
    for (;;) sleep(3600); /* main only boots; the tasks run on their own workers */
}

/* ---- the page's controls ---- */
EMSCRIPTEN_KEEPALIVE const uint8_t *pal_web_frame(void) { return s_rgba; }
EMSCRIPTEN_KEEPALIVE int pal_web_frame_counter(void) { return s_frame; }
EMSCRIPTEN_KEEPALIVE int pal_web_width(void) { return W; }
EMSCRIPTEN_KEEPALIVE int pal_web_height(void) { return H; }

static void inject(uint8_t sym, uint32_t cp, uint8_t mods) {
    pal_key_t k = {sym, mods, false, cp};
    pal_input_inject(&k);
}

/* A resolved key: sym is a PAL_KEY_*, cp the code point for PAL_KEY_CHAR. */
EMSCRIPTEN_KEEPALIVE void pal_web_key(int sym, unsigned cp, int mods) { inject((uint8_t)sym, cp, (uint8_t)mods); }

/* The same names as the native simulator's --key: enter esc tab backspace up down left right home end pgup pgdn delete switch
 * home-app vkb fntap, ctrl-alt-esc, ctrl-X and alt-X. Returns 0, or -1 for an unknown name. */
EMSCRIPTEN_KEEPALIVE int pal_web_key_named(const char *n) {
    static const struct { const char *name; uint8_t sym; } K[] = {
        {"enter", PAL_KEY_ENTER}, {"esc", PAL_KEY_ESC}, {"tab", PAL_KEY_TAB}, {"backspace", PAL_KEY_BACKSPACE},
        {"up", PAL_KEY_UP}, {"down", PAL_KEY_DOWN}, {"left", PAL_KEY_LEFT}, {"right", PAL_KEY_RIGHT},
        {"home", PAL_KEY_HOME}, {"end", PAL_KEY_END}, {"pgup", PAL_KEY_PGUP}, {"pgdn", PAL_KEY_PGDN},
        {"delete", PAL_KEY_DELETE}, {"switch", PAL_KEY_SWITCH}, {"home-app", PAL_KEY_GOHOME}, {"vkb", PAL_KEY_VKB},
        {"fntap", PAL_KEY_FNTAP}, {"insert", PAL_KEY_INSERT},
    };
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++)
        if (!strcmp(K[i].name, n)) return inject(K[i].sym, 0, 0), 0;
    if (!strcmp(n, "ctrl-alt-esc")) return inject(PAL_KEY_ESC, 0, PAL_MOD_CTRL | PAL_MOD_ALT), 0; /* the system console's switch key */
    if (!strncmp(n, "ctrl-", 5) && n[5]) return inject(PAL_KEY_CHAR, (unsigned char)n[5], PAL_MOD_CTRL), 0;
    if (!strncmp(n, "alt-", 4) && n[4]) return inject(PAL_KEY_CHAR, (unsigned char)n[4], PAL_MOD_ALT), 0;
    return -1;
}

/* UTF-8 text typed as characters; a newline is Enter, a tab is Tab. */
EMSCRIPTEN_KEEPALIVE void pal_web_type(const char *s) {
    for (uint32_t cp; (cp = pal_utf8_next(&s)) != 0;) {
        if (cp == '\n') inject(PAL_KEY_ENTER, 0, 0);
        else if (cp == '\t') inject(PAL_KEY_TAB, 0, 0);
        else inject(PAL_KEY_CHAR, cp, 0);
    }
}

/* A physical key of the 4x10 matrix: the keyboard layers (Fn, Nm, Shift, the long press of the cycle key) work as on the board. */
EMSCRIPTEN_KEEPALIVE void pal_web_hw(int row, int col, int pressed) { pal_input_hw(row, col, pressed != 0); }

/* Simulated hardware, changeable while the system runs. */
EMSCRIPTEN_KEEPALIVE void pal_web_set_battery(int pct) { sim_hw_set_battery(pct); }
EMSCRIPTEN_KEEPALIVE int pal_web_led(int i) {
    uint8_t c[3] = {0, 0, 0};
    sim_hw_led_get(i, c);
    return (c[0] << 16) | (c[1] << 8) | c[2];
}
EMSCRIPTEN_KEEPALIVE void pal_web_sensor_set(int i, int milli) { sim_hw_sensor_set(i, milli); }
EMSCRIPTEN_KEEPALIVE int pal_web_gpio_get(int i) { return sim_hw_gpio_get(i); }
EMSCRIPTEN_KEEPALIVE void pal_web_gpio_set(int i, int level) { sim_hw_gpio_set(i, level); }
EMSCRIPTEN_KEEPALIVE void pal_web_i2c_set(int addr, int present) { sim_hw_i2c_set(addr, present != 0); }

/* Wi-Fi: the networks a scan sees, and how connecting behaves. */
EMSCRIPTEN_KEEPALIVE void pal_web_ap_clear(void) { sim_hw_ap_clear(); }
EMSCRIPTEN_KEEPALIVE int pal_web_ap_add(const char *ssid, int rssi, int channel, int secure, const char *password) {
    return sim_hw_ap_add(ssid, rssi, channel, secure != 0, password);
}
EMSCRIPTEN_KEEPALIVE void pal_web_wifi_config(const char *ip, int refuse) { sim_hw_wifi_config(ip, refuse != 0); }
EMSCRIPTEN_KEEPALIVE int pal_web_wifi_connected(void) { return sim_hw_wifi_connected(); }
EMSCRIPTEN_KEEPALIVE void pal_web_wifi_drop(void) { sim_hw_wifi_set_connected(false); }

/* The radios the system runs: 0 = Wi-Fi, 1 = Bluetooth LE; a pal_radio_state_t (0 off, 1 starting, 2 on, 3 stopping, 4 failed). */
EMSCRIPTEN_KEEPALIVE int pal_web_radio_state(int radio) { return (int)pal_radio_state((pal_radio_t)radio); }
