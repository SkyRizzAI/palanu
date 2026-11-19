/* Settings (view id "settings"): the root page of the reference grouping, the generic row callbacks that read and write settings
 * keys through the table (psys_settings_set), and the view that hosts the menu. The pages live in settings_*.c; the
 * background jobs in settings_job.c. docs/UI.md describes the pages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "libapp.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/desktops.h"
#include "pal/menu.h"
#include "pal/settings_schema.h"
#include "pal/settings.h"
#include "pal/theme.h"
#include "pal/toast.h"
#include "settings_int.h"

static pal_menu_t M;
static bool s_inited;
static char s_route[24];
static bool s_route_pending;

/* ---- the root page: the reference groups, with PalanuOS's additions ---- */

/* The root in the pre-port groups (9471f26): SCREEN, CONNECTIONS, DEVICE and SYSTEM, with the reference entries inside them. Every
 * page is reached from here or from a page below it (Device & Hardware holds Sounds, Camera, Touch, LEDs, Sensors and
 * Battery; Developer holds the rest). */
/* GROUP: a boundary between groups without a title; a session that draws cards (Athena) starts a new one, the classic list skips it. */
#define GROUP {.kind = PAL_MENU_SECTION, .group = true}
static const pal_menu_row_t ROOT_ROWS[] = {
    SET_SUB("Desktop", "apps", PG_SESSION), /* first: which desktop runs and its own settings */
    GROUP,
    SET_SUB("Display", "brightness", PG_DISPLAY),
    {.kind = PAL_MENU_SUBMENU, .label = "Appearances", .icon = "moon", .ctx = (void *)&PG_APPEARANCE, .open = settings_open_ctx,
     .owner = "classic"}, /* the classic session's look: left out under the others (ADR-0010 §7) */
    SET_SUB("Controls", "terminal", PG_CONTROLS),
    GROUP,
    SET_SUB("Wi-Fi", "wifi", PG_WIFI),
    SET_SUB("Bluetooth", "bluetooth", PG_BLUETOOTH),
    SET_SUB("Remote", "link", PG_REMOTE),
    GROUP,
    SET_SUB("Device & Hardware", "default", PG_DEVICE),
    SET_SUB("Notifications", "notifications", PG_NOTIFY),
    SET_SUB("Storage", "files", PG_STORAGE),
    SET_SUB("Apps", "store", PG_APPS),
    GROUP,
    SET_SUB("Profile", "lock", PG_PROFILE),
    SET_SUB("Date & time", "clock", PG_DATETIME),
    SET_SUB("Updates", "power", PG_UPDATES),
    GROUP,
    SET_SUB("Developer", "monitor", PG_DEVELOPER),
    SET_SUB("About", "info", PG_ABOUT),
};

/* Device & Hardware: a section header, then Sounds, LEDs and Battery as the reference
 * board lists them. Camera, Touch and Sensors are listed only when the board has the capability (the reference lists them the same
 * way); Secure Element and Bluetooth are reached from their routes (uishow settings NAME) and from Developer > System. */
static pal_menu_row_t DEVICE_STORE[8] NEMA_EXT_BSS;

static void add_sub(pal_menu_page_t *p, const char *label, const char *icon, pal_menu_page_t *pg) {
    pal_menu_row_t r = {.kind = PAL_MENU_SUBMENU, .label = label, .icon = icon, .ctx = (void *)pg, .open = settings_open_ctx};
    pal_menu_add(p, &r);
}

static void build_device(pal_menu_page_t *p) {
    pal_menu_clear(p);
    add_sub(p, "Sounds", "default", &PG_SOUND);
    if (pal_has(PAL_CAP_CAMERA)) add_sub(p, "Camera", "default", &PG_CAMERA);
    if (pal_has(PAL_CAP_TOUCH)) add_sub(p, "Touch", "default", &PG_TOUCH);
    add_sub(p, "LEDs", "default", &PG_LED);
    if (pal_has(PAL_CAP_SENSORS)) add_sub(p, "Sensors", "default", &PG_SENSORS);
    add_sub(p, "Battery", "battery", &PG_BATTERY);
}

/* Desktop: the session (switches now) and the running session's own settings, laid out from the settings schema. */
static pal_menu_row_t SESSION_STORE[16] NEMA_EXT_BSS;

static void build_session(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_settings_schema_rows(p, NULL, "Desktop");
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "This desktop"};
    pal_menu_add(p, &r);
    pal_settings_schema_rows(p, pal_desktops_active(), NULL);
}

pal_menu_page_t PG_SESSION = {.title = "Desktop", .store = SESSION_STORE, .cap = 16, .build = build_session};

pal_menu_page_t PG_DEVICE = {.title = "Device & Hardware", .store = DEVICE_STORE, .cap = 8, .build = build_device};

pal_menu_page_t PG_ROOT = {.title = NULL, .rows = ROOT_ROWS, .n = sizeof(ROOT_ROWS) / sizeof(ROOT_ROWS[0])};

/* ---- the links from a name (uishow settings NAME) to a page; parent is pushed first so Esc goes back ---- */

typedef struct {
    const char *name;
    pal_menu_page_t *page;
    pal_menu_page_t *parent;
} route_t;

static const route_t ROUTES[] = {
    {"display", &PG_DISPLAY, NULL},
    {"appearance", &PG_APPEARANCE, NULL},
    {"desktop-setting", &PG_DESKTOP_SETTING, &PG_APPEARANCE},
    {"controls", &PG_CONTROLS, NULL},
    {"keys", &PG_KEYS, &PG_DEVELOPER},
    {"keymap", &PG_KEYMAP, &PG_KEYS},
    {"wifi", &PG_WIFI, NULL},
    {"wifi-detail", &PG_WIFI_DETAIL, &PG_WIFI},
    {"wifi-scan", &PG_WIFI_SCAN, &PG_WIFI},
    {"wifi-saved", &PG_WIFI_SAVED, &PG_WIFI},
    {"bluetooth", &PG_BLUETOOTH, &PG_DEVICE},
    {"remote", &PG_REMOTE, NULL},
    {"remote-link", &PG_REMOTE_LINK, &PG_DEVELOPER},
    {"link-clients", &PG_LINK_CLIENTS, &PG_REMOTE_LINK},
    {"device", &PG_DEVICE, NULL},
    {"sound", &PG_SOUND, &PG_DEVICE},
    {"led", &PG_LED, &PG_DEVICE},
    {"battery", &PG_BATTERY, &PG_DEVICE},
    {"camera", &PG_CAMERA, &PG_DEVICE},
    {"touch", &PG_TOUCH, &PG_DEVICE},
    {"sensors", &PG_SENSORS, &PG_DEVICE},
    {"secure", &PG_SECURE, &PG_DEVICE},
    {"notifications", &PG_NOTIFY, &PG_DEVELOPER},
    {"notify-apps", &PG_NOTIFY_APPS, &PG_NOTIFY},
    {"power", &PG_POWER, &PG_DEVELOPER},
    {"storage", &PG_STORAGE, NULL},
    {"storage-detail", &PG_STORAGE_DETAIL, &PG_DEVELOPER},
    {"apps", &PG_APPS, NULL},
    {"app-detail", &PG_APP_DETAIL, &PG_APPS},
    {"app-terminal", &PG_APP_DETAIL, &PG_APPS}, /* the Terminal app's detail: settings_apps_select("terminal") */
    {"services", &PG_SERVICES, &PG_DEVELOPER},
    {"service-detail", &PG_SERVICE_DETAIL, &PG_SERVICES},
    {"datetime", &PG_DATETIME, &PG_DEVELOPER},
    {"profile", &PG_PROFILE, NULL},
    {"pin", &PG_PIN, &PG_PROFILE},
    {"updates", &PG_UPDATES, &PG_DEVELOPER},
    {"developer", &PG_DEVELOPER, NULL},
    {"about", &PG_ABOUT, NULL},
    {"desktop", &PG_SESSION, NULL},
    {"about-detail", &PG_ABOUT_DETAIL, &PG_DEVELOPER},
    {"about-notices", &PG_ABOUT_NOTICES, &PG_ABOUT_DETAIL},
};

pal_menu_page_t *settings_page_by_name(const char *name) {
    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++)
        if (!strcmp(ROUTES[i].name, name)) return ROUTES[i].page;
    return NULL;
}

static void route_now(void) {
    pal_menu_t *m = settings_menu();
    pal_menu_reset(m, &PG_ROOT);
    if (!strncmp(s_route, "app-", 4) && strcmp(s_route, "app-detail")) settings_apps_select(s_route + 4);
    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        if (strcmp(ROUTES[i].name, s_route)) continue;
        if (ROUTES[i].parent) pal_menu_push(m, ROUTES[i].parent);
        pal_menu_push(m, ROUTES[i].page);
        return;
    }
}

/* uishow settings NAME: opens the view on that page. Applied by the view's pump on the desktop task. */
void pal_settings_request_page(const char *name) {
    snprintf(s_route, sizeof(s_route), "%s", name ? name : "root");
    s_route_pending = true;
}

/* ---- helpers shared by the pages ---- */

pal_menu_page_t *settings_open_ctx(void *ctx) { return (pal_menu_page_t *)ctx; }

pal_menu_t *settings_menu(void) {
    if (!s_inited) {
        pal_menu_init(&M, &PG_ROOT);
        s_inited = true;
    }
    return &M;
}

void settings_toast(const char *text) { pal_menu_toast(text); }

void settings_text_static(void *ctx, char *out, size_t cap) { snprintf(out, cap, "%s", (const char *)ctx); }

void settings_confirm(const char *title, const char *text, const char *ok, void (*done)(void *ctx, bool ok), void *ctx) {
    pal_menu_confirm(settings_menu(), title, text, ok, done, ctx);
}

/* Writes a key through the table; a refusal is said in a toast (the value on the screen does not change). */
static bool write_key(const char *key, const char *value) {
    int rc = psys_settings_set(key, value);
    if (rc == NEMA_OK) return true;
    char msg[48];
    snprintf(msg, sizeof(msg), "Not set: %s", pal_strerror(rc));
    settings_toast(msg);
    return false;
}

void settings_key_text(const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!key || psys_settings_get(key, out, cap) < 0) out[0] = 0;
}

int settings_key_int(const char *key, int def) {
    char t[24];
    settings_key_text(key, t, sizeof(t));
    if (!t[0]) return def;
    return atoi(t);
}

/* ---- the generic row callbacks ---- */

int settings_bool_get(void *ctx) { return settings_key_int((const char *)ctx, 0) != 0; }
void settings_bool_set(void *ctx, int v) {
    char t[4];
    snprintf(t, sizeof(t), "%d", v ? 1 : 0);
    (void)write_key((const char *)ctx, t);
}

int settings_int_get(void *ctx) { return settings_key_int((const char *)ctx, 0); }
void settings_int_set(void *ctx, int v) {
    char t[16];
    snprintf(t, sizeof(t), "%d", v);
    (void)write_key((const char *)ctx, t);
}

int settings_map_get(void *ctx) {
    const settings_map_t *m = (const settings_map_t *)ctx;
    char cur[40];
    settings_key_text(m->key, cur, sizeof(cur));
    for (int i = 0; i < m->n; i++) {
        if (m->texts && !strcmp(m->texts[i], cur)) return i;
        if (m->ints && m->ints[i] == atoi(cur) && cur[0]) return i;
    }
    return 0;
}

void settings_map_set(void *ctx, int index) {
    const settings_map_t *m = (const settings_map_t *)ctx;
    if (index < 0 || index >= m->n) return;
    char t[16];
    if (m->texts) (void)write_key(m->key, m->texts[index]);
    else {
        snprintf(t, sizeof(t), "%d", (int)m->ints[index]);
        (void)write_key(m->key, t);
    }
}

void settings_text_get(void *ctx, char *out, size_t cap) { settings_key_text((const char *)ctx, out, cap); }

bool settings_text_apply(void *ctx, const char *text) { return write_key((const char *)ctx, text); }

/* ---- background jobs: start with the busy overlay up; the result comes back in pump ---- */

bool settings_run_job(settings_job_t k, const char *a, const char *b, const char *busy) {
    if (!settings_job_start(k, a, b)) {
        settings_toast("Busy: wait for the last operation");
        return false;
    }
    pal_busy_show(busy);
    return true;
}

static void job_done(settings_job_t k, int rc, const char *arg) {
    pal_busy_hide();
    char msg[64];
    switch (k) {
    case JOB_WIFI_ON:
    case JOB_BLE_ON:
        if (rc == NEMA_OK) snprintf(msg, sizeof(msg), "%s is on", k == JOB_WIFI_ON ? "Wi-Fi" : "Bluetooth");
        else snprintf(msg, sizeof(msg), "Not on: %s", arg[0] ? arg : pal_strerror(rc));
        break;
    case JOB_WIFI_OFF:
        snprintf(msg, sizeof(msg), "Wi-Fi is off");
        break;
    case JOB_BLE_OFF:
        snprintf(msg, sizeof(msg), "Bluetooth is off");
        break;
    case JOB_SCAN:
        if (rc == NEMA_OK) msg[0] = 0;
        else snprintf(msg, sizeof(msg), "Scan failed: %s", arg[0] ? arg : pal_strerror(rc));
        break;
    case JOB_CONNECT:
        if (rc == NEMA_OK) snprintf(msg, sizeof(msg), "Connected to %s", arg);
        else snprintf(msg, sizeof(msg), "Not connected: %s", pal_strerror(rc));
        break;
    case JOB_DISCONNECT:
        snprintf(msg, sizeof(msg), rc == NEMA_OK ? "Disconnected" : "Disconnect failed");
        break;
    case JOB_FORGET:
        snprintf(msg, sizeof(msg), rc == NEMA_OK ? "Network forgotten" : "Could not forget");
        break;
    case JOB_SVC_START:
    case JOB_SVC_STOP:
    case JOB_SVC_RESTART:
        snprintf(msg, sizeof(msg), rc == NEMA_OK ? "%s %s: done" : "%s %s: failed", settings_job_name(k), arg);
        break;
    case JOB_REMOTE_ON:
    case JOB_REMOTE_OFF:
        snprintf(msg, sizeof(msg), rc == NEMA_OK ? (k == JOB_REMOTE_ON ? "Remote is on" : "Remote is off") : "Remote: not changed");
        pal_menu_refresh(settings_menu());
        break;
    case JOB_SD_MOUNT:
        snprintf(msg, sizeof(msg), rc == NEMA_OK ? "Card mounted at /sd" : "No card found");
        pal_menu_refresh(settings_menu());
        break;
    case JOB_SD_EJECT:
        snprintf(msg, sizeof(msg), "Card can be removed");
        pal_menu_refresh(settings_menu());
        break;
    case JOB_UPDATE_CHECK:
        snprintf(settings_update_text, sizeof(settings_update_text), "%s", rc == NEMA_OK ? arg : "Check failed");
        settings_update_ready = rc == NEMA_OK && strstr(arg, "available") != NULL;
        snprintf(msg, sizeof(msg), "%s", arg[0] ? arg : pal_strerror(rc));
        break;
    case JOB_UPDATE_INSTALL:
        if (rc == NEMA_OK) settings_update_ready = false, snprintf(settings_update_text, sizeof(settings_update_text), "Restart to finish");
        snprintf(msg, sizeof(msg), "%s", rc == NEMA_OK ? arg : arg[0] ? arg : pal_strerror(rc));
        break;
    case JOB_UNINSTALL:
        if (rc == NEMA_OK) {
            snprintf(msg, sizeof(msg), "Uninstalled %s", arg);
            if (pal_menu_top(settings_menu()) == &PG_APP_DETAIL) pal_menu_pop(settings_menu());
        } else snprintf(msg, sizeof(msg), "Not uninstalled: %s", pal_strerror(rc));
        break;
    default:
        msg[0] = 0;
        break;
    }
    if (msg[0]) settings_toast(msg);
    pal_menu_refresh(settings_menu());
    pal_desktop_invalidate();
}

/* ---- the view ---- */

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    pal_menu_draw(settings_menu(), c, a, pal_theme_active());
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    bool used = pal_menu_key(settings_menu(), k);
    if (used) pal_menu_follow(settings_menu());
    if (used) pal_desktop_invalidate();
    return used;
}

static void enter(pal_view_t *v) {
    (void)v;
    pal_menu_bind("settings", settings_menu()); /* a desktop may draw the pages in its own look (pal/menu.h) */
}

/* Every 250 ms: the selected row's long text scrolls, and a live page (values that move) repaints every other tick. */
static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    static unsigned n;
    pal_menu_page_t *p = pal_menu_top(settings_menu());
    bool paint = pal_menu_tick(settings_menu());
    if (p && p->live && (n++ & 1) == 0) paint = true;
    if (paint) pal_desktop_invalidate();
}

static void pump(pal_view_t *v) {
    (void)v;
    if (s_route_pending) {
        s_route_pending = false;
        route_now();
        pal_desktop_invalidate();
    }
    settings_job_t k;
    int rc;
    char arg[65];
    if (settings_job_take(&k, &rc, arg, sizeof(arg))) job_done(k, rc, arg);
}

pal_view_t pal_settings_view = {.id = "settings", .title = "Settings", .draw = draw, .key = key, .enter = enter,
                                .pump = pump, .tick = tick, .tick_ms = 250};
