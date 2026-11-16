/* PalanuOS host simulator.
 *
 *   palanu-sim                         interactive window (SDL2), keyboard typed directly
 *   palanu-sim --cmd "ls" --wait 300 --shot out.ppm   scripted, no window: steps run in order
 *
 * Steps: --type TEXT, --cmd LINE (type + Enter), --key NAME, --wait MS, --shot FILE.ppm, --text FILE
 * Keys: enter esc tab backspace up down left right home end pgup pgdn delete switch home-app vkb
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "builtin.h"
#include "nema/nema.h"
#include "pal/bootcount.h"
#include "pal/link.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/term.h"
#include "pal/wifi.h"

#ifdef HAVE_SDL
#include <SDL.h>
#endif

extern const pal_keyboard_t HHV2_KEYBOARD; /* the simulator types on the hhv2 key table */
extern const char *const HHV2_LEGEND_KEYS[]; /* and labels its legend bar with the hhv2 key caps */

#define W 320
#define H 240

/* --battery PCT (below) is the simulated charge; -1 (the default) reads as "cannot measure", so the status bar hides it. */
static int s_sim_battery = -1;
static int sim_battery_percent(void) { return s_sim_battery; }

/* --leds N gives the simulated board N single-pixel RGB LEDs (default 0) so the LED pages and the dev command can be tried with
 * more than one; each prints its colour on stderr when --led-log is given and is kept for the tests in s_led_color. */
#define SIM_MAX_LEDS 8
static pal_device_t s_sim_dev[SIM_MAX_LEDS];
static char s_sim_id[SIM_MAX_LEDS][8], s_sim_name[SIM_MAX_LEDS][12];
static uint8_t s_led_color[SIM_MAX_LEDS][3];
static bool s_led_log;
/* --sensors N, --gpios N and --i2c add simulated sensors (temp0.. reading 21.5 C, then 22.5 ...), pins (gpio0.. on pins 10..,
 * holding what was written) and an I2C bus (devices at 0x48 and 0x68), so the pages, the dev command and apps run without a board. */
#define SIM_MAX_EXTRA 8
static pal_device_t s_sim_all[1 + SIM_MAX_LEDS + 2 * SIM_MAX_EXTRA + 1];
static char s_sen_id[SIM_MAX_EXTRA][8], s_sen_name[SIM_MAX_EXTRA][16], s_gpio_id[SIM_MAX_EXTRA][8], s_gpio_name[SIM_MAX_EXTRA][12];
static int s_gpio_level[SIM_MAX_EXTRA];
static int sim_dev_battery(void *ctx) {
    (void)ctx;
    return s_sim_battery;
}
static int sim_sensor_read(void *ctx, int32_t *milli) {
    *milli = 21500 + 1000 * (int)(intptr_t)ctx;
    return 0;
}
static int sim_gpio_read(void *ctx) { return s_gpio_level[(intptr_t)ctx]; }
static int sim_gpio_write(void *ctx, int level) {
    s_gpio_level[(intptr_t)ctx] = level;
    if (s_led_log) fprintf(stderr, "sim: gpio%d = %d\n", (int)(intptr_t)ctx, level);
    return 0;
}
static int sim_bus_xfer(void *ctx, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn) {
    (void)ctx;
    if (addr != 0x48 && addr != 0x68) return -1;
    int reg = txn > 0 ? tx[0] : 0;
    for (int i = 0; i < rxn; i++) rx[i] = (uint8_t)(reg + i); /* a register file where register n holds n */
    return 0;
}
static int sim_i2c_probe(void *ctx, int addr) {
    (void)ctx;
    return addr == 0x48 || addr == 0x68;
}
static void sim_led_set(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    int i = (int)(intptr_t)ctx;
    (void)pixel;
    s_led_color[i][0] = r, s_led_color[i][1] = g, s_led_color[i][2] = b;
    if (s_led_log) fprintf(stderr, "sim: led%d = %u,%u,%u\n", i, r, g, b);
}

/* The simulator's board: a window and a keyboard, and a battery whose level comes from --battery. No buzzer, LED or radio,
 * so the system reports them as absent (beep prints or does nothing). */
static pal_board_desc_t SIM_BOARD = {
    .name = "sim",
    .chip = "host",
    .caps = PAL_CAP_DISPLAY | PAL_CAP_KEYBOARD | PAL_CAP_BATTERY,
    .display_w = W,
    .display_h = H,
    .display_format = PAL_PIXFMT_RGB565,
    .keyboard = &HHV2_KEYBOARD,
    .ops = {.battery_percent = sim_battery_percent},
    .legend_keys = HHV2_LEGEND_KEYS, /* the simulator types and labels the hhv2 keys */
    .ui_scale_q = 8,
};

static uint8_t g_rgb[W * H * 3];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_frame;

static void present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            uint16_t v = pal[px[y * stride + x]];
            v = (uint16_t)((v >> 8) | (v << 8)); /* the palette is byte-swapped for SPI */
            uint8_t *o = &g_rgb[(y * W + x) * 3];
            /* rounded, as the reference screenshots are (0x2928 -> #292442, 0xFB20 -> #FF6500) */
            o[0] = (uint8_t)((((v >> 11) & 31) * 255 + 15) / 31);
            o[1] = (uint8_t)((((v >> 5) & 63) * 255 + 31) / 63);
            o[2] = (uint8_t)(((v & 31) * 255 + 15) / 31);
        }
    }
    g_frame++;
    pthread_mutex_unlock(&g_lock);
}

static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    pthread_mutex_lock(&g_lock);
    fwrite(g_rgb, 1, sizeof(g_rgb), f);
    pthread_mutex_unlock(&g_lock);
    fclose(f);
}

/* The simulator has no radios: the manager's ops only flip the state, so the policy, admission and commands run as on
 * the board. Wi-Fi has no driver here (the wifi command reports it unavailable). */
static nema_err_t sim_radio_start(pal_radio_t r) {
    (void)r;
    return NEMA_OK;
}
static nema_err_t sim_radio_stop(pal_radio_t r) {
    (void)r;
    return NEMA_OK;
}
static const pal_radio_ops_t SIM_RADIO = {sim_radio_start, sim_radio_stop, NULL};

/* The simulator's Wi-Fi, for the Settings pages and the golden scenes: four fixed networks, and a connect that succeeds at once.
 * Nothing reaches a network, and the saved network is never kept (forget and a restart forget the join). */
static const pal_ap_t SIM_APS[] = {
    {"MyHomeWiFi", -42, 36, true},
    {"CoffeeShop_Free", -68, 6, false},
    {"Neighbour_5G", -74, 11, true},
    {"AndroidAP", -81, 1, true},
};
static char s_sim_ssid[33];
static bool s_sim_connected;

static nema_err_t sim_wifi_scan(pal_ap_t *out, size_t max, size_t *found) {
    size_t n = sizeof(SIM_APS) / sizeof(SIM_APS[0]);
    if (n > max) n = max;
    memcpy(out, SIM_APS, n * sizeof(pal_ap_t));
    *found = n;
    return NEMA_OK;
}
static nema_err_t sim_wifi_connect(const char *ssid, const char *pass, int32_t timeout_ms) {
    (void)pass, (void)timeout_ms;
    snprintf(s_sim_ssid, sizeof(s_sim_ssid), "%s", ssid);
    s_sim_connected = true;
    return NEMA_OK;
}
static nema_err_t sim_wifi_disconnect(void) {
    s_sim_connected = false;
    return NEMA_OK;
}
static nema_err_t sim_wifi_forget(void) {
    s_sim_connected = false;
    s_sim_ssid[0] = 0;
    return NEMA_OK;
}
static void sim_wifi_status(pal_wifi_status_t *st) {
    st->connected = s_sim_connected;
    st->saved = false;
    snprintf(st->ssid, sizeof(st->ssid), "%s", s_sim_ssid);
    snprintf(st->ip, sizeof(st->ip), "%s", s_sim_connected ? "192.168.1.20" : "");
    st->rssi = s_sim_connected ? -48 : 0;
    st->time_synced = s_sim_connected;
}
static bool sim_wifi_has_credentials(void) { return false; }
static const pal_wifi_ops_t SIM_WIFI = {sim_wifi_scan, sim_wifi_connect, sim_wifi_disconnect, sim_wifi_forget,
                                        sim_wifi_status, sim_wifi_has_credentials};

/* Host-only fixtures for the golden scenes: --battery PCT sets the level the status bar shows (-1 keeps the board's
 * "no battery"); the Wi-Fi icon follows the simulated connection. */
static pal_status_t status(void) {
    int bat = s_sim_battery >= 0 ? s_sim_battery : pal_battery();
    pal_status_t s = {0, 0, bat, s_sim_connected ? 3 : 0, pal_link_connected(), false, false};
    return s;
}

static void inject(uint8_t sym, uint32_t cp, uint8_t mods) {
    pal_key_t k = {sym, mods, false, cp};
    pal_input_inject(&k);
}

static void type_text(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') inject(PAL_KEY_ENTER, 0, 0);
        else if (*s == '\t') inject(PAL_KEY_TAB, 0, 0);
        else inject(PAL_KEY_CHAR, (unsigned char)*s, 0);
        usleep(4000);
    }
}

static void key_named(const char *n) {
    static const struct { const char *name; uint8_t sym; } K[] = {
        {"enter", PAL_KEY_ENTER}, {"esc", PAL_KEY_ESC}, {"tab", PAL_KEY_TAB}, {"backspace", PAL_KEY_BACKSPACE},
        {"up", PAL_KEY_UP}, {"down", PAL_KEY_DOWN}, {"left", PAL_KEY_LEFT}, {"right", PAL_KEY_RIGHT},
        {"home", PAL_KEY_HOME}, {"end", PAL_KEY_END}, {"pgup", PAL_KEY_PGUP}, {"pgdn", PAL_KEY_PGDN},
        {"delete", PAL_KEY_DELETE}, {"switch", PAL_KEY_SWITCH}, {"home-app", PAL_KEY_GOHOME}, {"vkb", PAL_KEY_VKB}, {"fntap", PAL_KEY_FNTAP},
    };
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++)
        if (strcmp(K[i].name, n) == 0) {
            inject(K[i].sym, 0, 0);
            return;
        }
    if (strncmp(n, "ctrl-", 5) == 0 && n[5]) inject(PAL_KEY_CHAR, (unsigned char)n[5], PAL_MOD_CTRL);
    else if (strncmp(n, "alt-", 4) == 0 && n[4]) inject(PAL_KEY_CHAR, (unsigned char)n[4], PAL_MOD_ALT);
    else fprintf(stderr, "unknown key %s\n", n);
}

static int run_script(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--type") && i + 1 < argc) type_text(argv[++i]);
        else if (!strcmp(argv[i], "--cmd") && i + 1 < argc) {
            type_text(argv[++i]);
            inject(PAL_KEY_ENTER, 0, 0);
        } else if (!strcmp(argv[i], "--key") && i + 1 < argc) key_named(argv[++i]);
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) usleep((useconds_t)atoi(argv[++i]) * 1000u);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) save_ppm(argv[++i]);
        else if (!strcmp(argv[i], "--battery") && i + 1 < argc) s_sim_battery = atoi(argv[++i]);
    }
    return 0;
}

#ifdef HAVE_SDL
static void sdl_key(const SDL_KeyboardEvent *e) {
    uint8_t mods = 0;
    if (e->keysym.mod & KMOD_SHIFT) mods |= PAL_MOD_SHIFT;
    if (e->keysym.mod & KMOD_CTRL) mods |= PAL_MOD_CTRL;
    if (e->keysym.mod & KMOD_ALT) mods |= PAL_MOD_ALT;
    SDL_Keycode k = e->keysym.sym;
    switch (k) {
    case SDLK_RETURN: inject(PAL_KEY_ENTER, 0, mods); return;
    case SDLK_ESCAPE: inject(PAL_KEY_ESC, 0, mods); return;
    case SDLK_BACKSPACE: inject(PAL_KEY_BACKSPACE, 0, mods); return;
    case SDLK_TAB: inject(PAL_KEY_TAB, 0, mods); return;
    case SDLK_UP: inject(PAL_KEY_UP, 0, mods); return;
    case SDLK_DOWN: inject(PAL_KEY_DOWN, 0, mods); return;
    case SDLK_LEFT: inject(PAL_KEY_LEFT, 0, mods); return;
    case SDLK_RIGHT: inject(PAL_KEY_RIGHT, 0, mods); return;
    case SDLK_HOME: inject(PAL_KEY_HOME, 0, mods); return;
    case SDLK_END: inject(PAL_KEY_END, 0, mods); return;
    case SDLK_PAGEUP: inject(PAL_KEY_PGUP, 0, mods); return;
    case SDLK_PAGEDOWN: inject(PAL_KEY_PGDN, 0, mods); return;
    case SDLK_DELETE: inject(PAL_KEY_DELETE, 0, mods); return;
    case SDLK_F1: inject(PAL_KEY_SWITCH, 0, 0); return;
    case SDLK_F2: inject(PAL_KEY_GOHOME, 0, 0); return;
    default: break;
    }
    if ((mods & (PAL_MOD_CTRL | PAL_MOD_ALT)) && k >= 32 && k < 127) inject(PAL_KEY_CHAR, (uint32_t)k, mods);
}

static int run_window(void) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("PalanuOS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * 3, H * 3, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, W, H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_StartTextInput();
    bool quit = false;
    int shown = -1;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            else if (e.type == SDL_KEYDOWN) sdl_key(&e.key);
            else if (e.type == SDL_TEXTINPUT) {
                const char *s = e.text.text;
                if (!(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT)))
                    for (uint32_t cp; (cp = pal_utf8_next(&s)) != 0;) inject(PAL_KEY_CHAR, cp, 0);
            }
        }
        if (g_frame != shown) {
            pthread_mutex_lock(&g_lock);
            SDL_UpdateTexture(tex, NULL, g_rgb, W * 3);
            shown = g_frame;
            pthread_mutex_unlock(&g_lock);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
    return 0;
}
#endif

/* The host's boot-attempt counter: a file in the data directory. Scripted runs (any --flag) are tests, not boots,
 * so they never touch it; the window simulator counts like the board does. */
static char g_bootcount_path[300];

static uint32_t host_bootcount_get(void) {
    FILE *f = fopen(g_bootcount_path, "r");
    unsigned long n = 0;
    if (f) {
        if (fscanf(f, "%lu", &n) != 1) n = 0;
        fclose(f);
    }
    return (uint32_t)n;
}

static nema_err_t host_bootcount_set(uint32_t n) {
    FILE *f = fopen(g_bootcount_path, "w");
    if (!f) return NEMA_ERR_IO;
    fprintf(f, "%lu\n", (unsigned long)n);
    fclose(f);
    return NEMA_OK;
}

static const pal_bootcount_ops_t HOST_BOOTCOUNT = {host_bootcount_get, host_bootcount_set};

/* The simulator's board services: the keyboard is started by boot, so input is core and already running. */
static void sim_services(void) {
    psys_svc_builtin_t b;
    memset(&b, 0, sizeof(b));
    b.id = "input";
    b.name = "Keyboard input";
    b.restart = PSYS_SVC_RESTART_NEVER;
    b.autostart = true;
    b.core = true;
    psys_svc_register_builtin(&b);
}

static bool has_arg(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], flag)) return true;
    return false;
}

static int arg_int(int argc, char **argv, const char *flag) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], flag)) return atoi(argv[i + 1]);
    return 0;
}

/* The memory card: the directory sdcard in the data directory stands for a card in the slot (made by hand, or by a test).
 * Without it the simulator has no card slot, as before. */
static char s_sd_dir[300];
static bool s_sd_on;
static const char *sim_sd_mount(void) {
    struct stat st;
    if (stat(s_sd_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    s_sd_on = true;
    return s_sd_dir;
}
static void sim_sd_unmount(void) { s_sd_on = false; }
static bool sim_sd_info(pal_sd_info_t *out) {
    if (!s_sd_on) return false;
    snprintf(out->name, sizeof(out->name), "SIMSD");
    out->size_bytes = 0;
    out->speed_khz = 0;
    return true;
}
static const pal_sd_ops_t SIM_SD = {sim_sd_mount, sim_sd_unmount, sim_sd_info, NULL};

int main(int argc, char **argv) {
    const char *data = getenv("PALANU_DATA");
    char dir[256];
    if (!data) {
        snprintf(dir, sizeof(dir), "%s/.palanu-sim", getenv("HOME") ? getenv("HOME") : ".");
        data = dir;
    }
    signal(SIGPIPE, SIG_IGN); /* a client that vanished must not kill the simulator */
    mkdir(data, 0777);
    s_led_log = has_arg(argc, argv, "--led-log");
    int nled = arg_int(argc, argv, "--leds"), nsen = arg_int(argc, argv, "--sensors"), ngpio = arg_int(argc, argv, "--gpios");
    if (nled > SIM_MAX_LEDS) nled = SIM_MAX_LEDS;
    if (nsen > SIM_MAX_EXTRA) nsen = SIM_MAX_EXTRA;
    if (ngpio > SIM_MAX_EXTRA) ngpio = SIM_MAX_EXTRA;
    bool bus = has_arg(argc, argv, "--i2c");
    if (nled > 0 || nsen > 0 || ngpio > 0 || bus) {
        int n = 0;
        s_sim_all[n++] = (pal_device_t){.id = "battery0", .name = "Battery", .kind = PAL_DEV_BATTERY, .battery = {.percent = sim_dev_battery}};
        for (int k = 0; k < nled; k++) {
            snprintf(s_sim_id[k], sizeof(s_sim_id[k]), "led%d", k);
            snprintf(s_sim_name[k], sizeof(s_sim_name[k]), "LED %d", k + 1);
            s_sim_all[n++] = (pal_device_t){.id = s_sim_id[k], .name = s_sim_name[k], .kind = PAL_DEV_LED, .ctx = (void *)(intptr_t)k,
                                            .led = {.pixels = 1, .colors = 3, .set = sim_led_set}};
        }
        for (int k = 0; k < nsen; k++) {
            snprintf(s_sen_id[k], sizeof(s_sen_id[k]), "temp%d", k);
            snprintf(s_sen_name[k], sizeof(s_sen_name[k]), "Temperature %d", k + 1);
            s_sim_all[n++] = (pal_device_t){.id = s_sen_id[k], .name = s_sen_name[k], .kind = PAL_DEV_SENSOR, .ctx = (void *)(intptr_t)k,
                                            .sensor = {.unit = "C", .read = sim_sensor_read}};
        }
        for (int k = 0; k < ngpio; k++) {
            snprintf(s_gpio_id[k], sizeof(s_gpio_id[k]), "gpio%d", k);
            snprintf(s_gpio_name[k], sizeof(s_gpio_name[k]), "Pin %d", 10 + k);
            s_sim_all[n++] = (pal_device_t){.id = s_gpio_id[k], .name = s_gpio_name[k], .kind = PAL_DEV_GPIO, .ctx = (void *)(intptr_t)k,
                                            .gpio = {.pin = 10 + k, .app = true, .read = sim_gpio_read, .write = sim_gpio_write}};
        }
        if (bus) s_sim_all[n++] = (pal_device_t){.id = "i2c0", .name = "I2C bus", .kind = PAL_DEV_BUS,
                                                 .bus = {.type = PAL_BUS_I2C, .app = true, .probe = sim_i2c_probe, .xfer = sim_bus_xfer}};
        SIM_BOARD.devices = s_sim_all, SIM_BOARD.ndevices = n;
    }
    pal_boot_cfg_t cfg = {
        .board = &SIM_BOARD,
        .storage_root = data,
        .display = {.present = present},
        .console_log = has_arg(argc, argv, "--log"), /* --log: the device log on stdout (tests read the pairing code) */
        .status = status,
        .bootcount = NULL,
        .platform_services = sim_services,
    };
    snprintf(s_sd_dir, sizeof(s_sd_dir), "%s/sdcard", data);
    struct stat sd_st;
    if (stat(s_sd_dir, &sd_st) == 0 && S_ISDIR(sd_st.st_mode)) cfg.sd = &SIM_SD;
    bool scripted = false;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--", 2)) scripted = true;
    if (!scripted) {
        snprintf(g_bootcount_path, sizeof(g_bootcount_path), "%s/bootcount", data);
        cfg.bootcount = &HOST_BOOTCOUNT;
    }
    pal_radio_set_ops(&SIM_RADIO);
    pal_wifi_set_ops(&SIM_WIFI);
    pal_boot(&cfg);
    /* --link-open trusts the TCP link without a token. Tests only: the default is an untrusted link (docs/PLP.md). */
    pal_link_set_tcp_trust(has_arg(argc, argv, "--link-open"));
    if (has_arg(argc, argv, "--link-open")) fprintf(stderr, "palanu-sim: --link-open: TCP link trusted without pairing (tests only)\n");
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--link")) pal_link_serve_tcp((uint16_t)atoi(argv[i + 1]));
        if (!strcmp(argv[i], "--ws")) pal_link_serve_ws((uint16_t)atoi(argv[i + 1])); /* Web Forge: ws://127.0.0.1:PORT */
    }
    if (scripted) {
        usleep(400000); /* let the desktop and the first shell come up */
        run_script(argc, argv);
        return 0;
    }
#ifdef HAVE_SDL
    return run_window();
#else
    fprintf(stderr, "built without SDL2: use scripted mode (--cmd, --shot)\n");
    return 1;
#endif
}
