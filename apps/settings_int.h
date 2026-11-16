/* Settings internals (apps/settings*.c): the page objects, the generic row callbacks that read and write settings keys, and the
 * background jobs. A page is a pal_menu_page_t defined in one settings_*.c file; the root (settings.c) links to them through
 * the SUB rows. Every change of a key goes through psys_settings_set (settings_api.c), never around it. */
#ifndef PAL_SETTINGS_INT_H
#define PAL_SETTINGS_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal/menu.h"
#include "pal/wifi.h"

/* ---- generic callbacks (settings.c). The ctx of a row is its key (a string), or a settings_map_t for a choice. ---- */

/* A choice that stores a text (texts) or an integer (ints) under key; labels are the row's choices. */
typedef struct {
    const char *key;
    const char *const *texts; /* stored names; NULL when ints is used */
    const int32_t *ints;      /* stored values */
    int n;
} settings_map_t;

int settings_bool_get(void *ctx);            /* ctx: key */
void settings_bool_set(void *ctx, int v);
int settings_int_get(void *ctx);             /* ctx: key */
void settings_int_set(void *ctx, int v);
int settings_map_get(void *ctx);             /* ctx: settings_map_t* */
void settings_map_set(void *ctx, int index);
void settings_text_get(void *ctx, char *out, size_t cap); /* ctx: key */
bool settings_text_apply(void *ctx, const char *text);    /* ctx: key; false when the table refuses the text */

/* The text of a key (empty when unknown) and the bool/int readers used by the info rows. */
void settings_key_text(const char *key, char *out, size_t cap);
int settings_key_int(const char *key, int def);

/* The page links and small helpers used by the SUB rows. open returns the page in ctx. */
pal_menu_page_t *settings_open_ctx(void *ctx);
pal_menu_t *settings_menu(void);
void settings_toast(const char *text);
/* Asks the desktop for a confirm dialog; done runs with the answer on the desktop task. */
void settings_confirm(const char *title, const char *text, const char *ok, void (*done)(void *ctx, bool ok), void *ctx);

/* ---- background jobs (settings_job.c): radio and Wi-Fi operations that block for a second or more run on a worker
 * task; the desktop polls the result in the view's pump. One job at a time. */
typedef enum {
    JOB_NONE = 0,
    JOB_WIFI_ON,
    JOB_WIFI_OFF,
    JOB_BLE_ON,
    JOB_BLE_OFF,
    JOB_SCAN,
    JOB_CONNECT,    /* a: SSID, b: password */
    JOB_DISCONNECT,
    JOB_FORGET,
    JOB_SVC_START,  /* a: service id */
    JOB_SVC_STOP,
    JOB_SVC_RESTART,
    JOB_UNINSTALL,  /* a: app id */
    JOB_SD_MOUNT,   /* the card's start-up can take a second without a card */
    JOB_SD_EJECT,
    JOB_REMOTE_ON,  /* the Palanu Link servers over TCP and WebSocket */
    JOB_REMOTE_OFF,
} settings_job_t;

bool settings_job_start(settings_job_t k, const char *a, const char *b);
bool settings_job_busy(void);
/* The finished job, once: true with its kind and result (nema_err_t, or 0). */
bool settings_job_take(settings_job_t *k, int *rc, char *arg, size_t cap);
const char *settings_job_name(settings_job_t k);

/* The last Wi-Fi scan (settings_job.c): copies up to max networks into out and returns how many. */
size_t settings_scan_get(pal_ap_t *out, size_t max);
bool settings_scan_done(void); /* a scan has finished at least once */

/* ---- pages (one per settings_*.c file) ---- */
extern pal_menu_page_t PG_DISPLAY, PG_APPEARANCE, PG_DESKTOP_SETTING, PG_CONTROLS, PG_KEYS, PG_KEYMAP;
extern pal_menu_page_t PG_WIFI, PG_WIFI_DETAIL, PG_WIFI_SCAN;
extern pal_menu_page_t PG_BLUETOOTH, PG_REMOTE, PG_REMOTE_LINK, PG_LINK_CLIENTS;
extern pal_menu_page_t PG_NOTIFY, PG_NOTIFY_APPS, PG_SOUND, PG_BATTERY, PG_POWER, PG_STORAGE, PG_STORAGE_DETAIL;
extern pal_menu_page_t PG_LED, PG_CAMERA, PG_TOUCH, PG_SENSORS, PG_SECURE;
extern pal_menu_page_t PG_APPS, PG_APP_DETAIL, PG_SERVICES, PG_SERVICE_DETAIL, PG_SVC_LOG;
extern pal_menu_page_t PG_DATETIME, PG_PROFILE, PG_PIN, PG_UPDATES, PG_DEVELOPER, PG_ABOUT, PG_ABOUT_DETAIL, PG_ABOUT_NOTICES;
extern pal_menu_page_t PG_ROOT, PG_DEVICE;

/* The app the detail pages show (uishow settings app-NAME, the golden scenes): set before the page is pushed. */
void settings_apps_select(const char *id);

/* Pair window overlay (settings_link.c): opens the 120 s pairing window and shows the code. */
void settings_pair_open(void);
/* The page a name opens (uishow settings NAME, the golden scenes): NULL when there is none. */
pal_menu_page_t *settings_page_by_name(const char *name);

/* Starts a background job with the busy overlay up (toast when it cannot start). */
bool settings_run_job(settings_job_t k, const char *a, const char *b, const char *busy);
/* Text callbacks: the ctx of an INFO row is a fixed string (settings_text_static), or a function of the page. */
void settings_text_static(void *ctx, char *out, size_t cap);

/* Wi-Fi helpers used by the Wi-Fi pages and the job results. */
void settings_wifi_connect_ask(const char *ssid, bool secure); /* asks the password when secure, then connects */

/* Row helpers (static initialisers). ctx is the page for SUB rows. */
#define SET_SECTION(lab) {.kind = PAL_MENU_SECTION, .label = (lab)}
#define SET_SUB(lab, ic, pg) {.kind = PAL_MENU_SUBMENU, .label = (lab), .icon = (ic), .ctx = (void *)&(pg), .open = settings_open_ctx}

#endif
