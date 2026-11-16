/* The browser simulator entry (Emscripten). The page loads this module, calls pal_web_start, then drives it with
 * pal_web_key / pal_web_type and reads the screen from pal_web_frame. The board is the simulator's: a window and the hhv2
 * keyboard. Everything else (Wi-Fi networks, devices, battery) is configured from the page through the pal_web_* calls. */
#include <emscripten.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtin.h"
#include "nema/nema.h"
#include "pal/board.h"
#include "pal/input.h"
#include "pal/svc.h"

#define W 320
#define H 240

extern const pal_keyboard_t HHV2_KEYBOARD;
extern const char *const HHV2_LEGEND_KEYS[];

static int s_battery = 80;
static int battery_percent(void) { return s_battery; }

static pal_board_desc_t BOARD = {
    .name = "web",
    .chip = "wasm",
    .caps = PAL_CAP_DISPLAY | PAL_CAP_KEYBOARD | PAL_CAP_BATTERY,
    .display_w = W,
    .display_h = H,
    .display_format = PAL_PIXFMT_RGB565,
    .keyboard = &HHV2_KEYBOARD,
    .ops = {.battery_percent = battery_percent},
    .legend_keys = HHV2_LEGEND_KEYS,
    .ui_scale_q = 8,
};

static uint8_t s_rgba[W * H * 4];
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

/* The page calls this once. Boot runs on the worker thread that main() lives on (PROXY_TO_PTHREAD). */
int main(void) {
    pal_boot_cfg_t cfg = {
        .board = &BOARD,
        .storage_root = "/data",
        .display = {.present = present},
        .console_log = true,
        .platform_services = services,
    };
    pal_boot(&cfg);
    for (;;) sleep(3600); /* main only boots; the tasks run on their own workers */
}

EMSCRIPTEN_KEEPALIVE const uint8_t *pal_web_frame(void) { return s_rgba; }
EMSCRIPTEN_KEEPALIVE int pal_web_frame_counter(void) { return s_frame; }
EMSCRIPTEN_KEEPALIVE int pal_web_width(void) { return W; }
EMSCRIPTEN_KEEPALIVE int pal_web_height(void) { return H; }
EMSCRIPTEN_KEEPALIVE void pal_web_key(int sym, unsigned cp, int mods) {
    pal_key_t k = {(uint8_t)sym, (uint8_t)mods, false, cp};
    pal_input_inject(&k);
}
EMSCRIPTEN_KEEPALIVE void pal_web_set_battery(int pct) { s_battery = pct; }
