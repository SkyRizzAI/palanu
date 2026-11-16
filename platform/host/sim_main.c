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
#include "sim_hw.h"
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

/* --battery PCT sets the simulated charge; -1 (the default) reads as "cannot measure", so the status bar hides it. The LEDs,
 * sensors, pins, I2C bus and the Wi-Fi are in sim_hw.c, shared with the browser simulator. */
static int sim_battery_percent(void) { return sim_hw_battery(); }

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

/* Host-only fixtures for the golden scenes: --battery PCT sets the level the status bar shows (-1 keeps the board's
 * "no battery"); the Wi-Fi icon follows the simulated connection. */
static pal_status_t status(void) {
    int bat = sim_hw_battery() >= 0 ? sim_hw_battery() : pal_battery();
    pal_status_t s = {0, 0, bat, sim_hw_wifi_connected() ? 3 : 0, pal_link_connected(), false, false};
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
        else if (!strcmp(argv[i], "--battery") && i + 1 < argc) sim_hw_set_battery(atoi(argv[++i]));
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
    sim_hw_cfg_t hw = {.leds = arg_int(argc, argv, "--leds"), .sensors = arg_int(argc, argv, "--sensors"),
                       .gpios = arg_int(argc, argv, "--gpios"), .i2c = has_arg(argc, argv, "--i2c"),
                       .log = has_arg(argc, argv, "--led-log")};
    sim_hw_init(&SIM_BOARD, &hw);
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
    pal_radio_set_ops(sim_hw_radio_ops());
    pal_wifi_set_ops(sim_hw_wifi_ops());
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
