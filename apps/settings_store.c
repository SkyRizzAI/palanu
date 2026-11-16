#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/board.h"
#include "pal/dpm.h"
#include "pal/settings.h"
#include "pal/theme.h"
#include "pal/time.h"

/* Settings live in /data/settings as "key=value" lines, so they survive a restart and can be edited by hand. */
#define SETTINGS_PATH "/data/settings"
#define SETTINGS_MAX 1536 /* the whole file, as saved and as read */

void pal_terminal_set_font(int f);
int pal_terminal_font(void);

/* Radio policy (docs/RADIO.md): radio.wifi is auto|off|on (auto = start at boot only with saved credentials),
 * radio.ble is off|on. Read at boot by pal_settings_load_radio(); written by the wifi and ble commands. */
static char s_wifi_mode[8] = "auto";
static char s_ble_mode[8] = "off";

const char *pal_settings_radio_wifi(void) { return s_wifi_mode; }
const char *pal_settings_radio_ble(void) { return s_ble_mode; }

void pal_settings_set_radio(const char *key, const char *value) {
    if (!strcmp(key, "radio.wifi")) nema_strlcpy(s_wifi_mode, value, sizeof(s_wifi_mode));
    else if (!strcmp(key, "radio.ble")) nema_strlcpy(s_ble_mode, value, sizeof(s_ble_mode));
    else return;
    pal_settings_save();
}

void pal_settings_save(void) {
    /* the text on the heap: 1.5 KB on the stack plus the file system's write is too much for the desktop task (6 KB),
     * which saves whenever a setting changes in Settings */
    char *text = (char *)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, SETTINGS_MAX), pin[100];
    if (!text) return;
    pal_dpm_pin_text(pin, sizeof(pin));
    int n = snprintf(text, SETTINGS_MAX, "theme=%s\ndark=%d\nsize=%d\nfont=%d\nbright=%d\ntz=%d\nradio.wifi=%s\nradio.ble=%s\ndpm.sleep=%u\ndpm.lock=%u\nsecurity.pin=%s\n",
                     pal_theme_name(pal_theme_index()), pal_theme_dark() ? 1 : 0, pal_theme_size(), pal_terminal_font(),
                     pal_display_brightness(), pal_time_tz_minutes(), s_wifi_mode, s_ble_mode,
                     (unsigned)(pal_dpm_sleep_ms() / 1000u), (unsigned)(pal_dpm_lock_ms() / 1000u), pin);
    if (n < 0 || n >= SETTINGS_MAX) {
        nema_free(text);
        return;
    }
    /* the keys with no owning module (dev.mode, boot.target, the Settings additions): one line each */
    size_t more = psys_settings_kv_format(text + n, SETTINGS_MAX - (size_t)n);
    nema_obj_t *f;
    if (nema_fs_open(SETTINGS_PATH, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) == NEMA_OK) {
        nema_obj_write(f, text, (size_t)n + more, 0);
        nema_obj_unref(f);
    }
    nema_free(text);
}

/* Reads the file and calls fn(key, value) for each line. Shared by the two loaders below. */
static void for_each_setting(void (*fn)(const char *k, const char *v)) {
    nema_obj_t *f;
    if (nema_fs_open(SETTINGS_PATH, NEMA_O_READ, &f) != NEMA_OK) return;
    char *buf = (char *)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, SETTINGS_MAX); /* not on the stack, as in the save */
    if (!buf) {
        nema_obj_unref(f);
        return;
    }
    int n = nema_obj_read(f, buf, SETTINGS_MAX - 1, 0);
    nema_obj_unref(f);
    if (n <= 0) {
        nema_free(buf);
        return;
    }
    buf[n] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        fn(line, eq + 1);
    }
    nema_free(buf);
}

static void take_radio(const char *k, const char *v) {
    if (!strcmp(k, "radio.wifi")) nema_strlcpy(s_wifi_mode, v, sizeof(s_wifi_mode));
    else if (!strcmp(k, "radio.ble")) nema_strlcpy(s_ble_mode, v, sizeof(s_ble_mode));
}

void pal_settings_load_radio(void) { for_each_setting(take_radio); }

static int s_theme, s_dark, s_size;

static void take_display(const char *k, const char *v) {
    if (!strcmp(k, "theme")) {
        for (int i = 0; i < pal_theme_count(); i++)
            if (!strcmp(pal_theme_name(i), v)) s_theme = i;
    } else if (!strcmp(k, "dark")) s_dark = atoi(v);
    else if (!strcmp(k, "size")) s_size = atoi(v);
    else if (!strcmp(k, "font")) pal_terminal_set_font(atoi(v));
    else if (!strcmp(k, "bright")) pal_display_set_brightness(atoi(v));
    else if (!strcmp(k, "tz")) pal_time_set_tz_minutes(atoi(v));
    else if (!strcmp(k, "dpm.sleep")) pal_dpm_set_sleep_ms((uint32_t)atoi(v) * 1000u);
    else if (!strcmp(k, "dpm.lock")) pal_dpm_set_lock_ms((uint32_t)atoi(v) * 1000u);
    else if (!strcmp(k, "security.pin")) pal_dpm_pin_load(v);
    else psys_settings_kv_load(k, v);
}

void pal_settings_load(void) {
    s_theme = pal_theme_index();
    s_dark = pal_theme_dark();
    s_size = pal_theme_size();
    nema_obj_t *f;
    if (nema_fs_open(SETTINGS_PATH, NEMA_O_READ, &f) != NEMA_OK) return;
    nema_obj_unref(f);
    for_each_setting(take_display);
    pal_theme_select(s_theme, s_dark != 0, s_size);
    psys_settings_kv_apply(); /* the effects of the loaded values (repeat, log level) */
}

/* ---- the system settings table (system/src/settings_api.c) reaches the values the apps keep ---- */

#define NAME_PATH "/data/device.name" /* the device name alone, as text: kept apart from the settings file */
#define NAME_MAX_LEN 31

static char s_name[NAME_MAX_LEN + 1] = "PalanuOS";

static void name_load(void) {
    nema_obj_t *f;
    if (nema_fs_open(NAME_PATH, NEMA_O_READ, &f) != NEMA_OK) return;
    char buf[NAME_MAX_LEN + 1];
    int n = nema_obj_read(f, buf, sizeof(buf) - 1, 0);
    nema_obj_unref(f);
    if (n <= 0) return;
    buf[n] = 0;
    if (strlen(buf) > 0) nema_strlcpy(s_name, buf, sizeof(s_name));
}

static int host_font_get(void) { return pal_terminal_font(); }
static void host_font_set(int f) { pal_terminal_set_font(f); }

static const char *host_radio_get(const char *key) {
    if (!strcmp(key, "radio.wifi")) return s_wifi_mode;
    if (!strcmp(key, "radio.ble")) return s_ble_mode;
    return NULL;
}

static const char *host_name_get(void) { return s_name; }

static int host_name_set(const char *name) {
    size_t n = strlen(name);
    nema_obj_t *f;
    if (nema_fs_open(NAME_PATH, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) return NEMA_ERR_IO;
    int w = nema_obj_write(f, name, n, 0);
    nema_obj_unref(f);
    if (w != (int)n) return NEMA_ERR_IO;
    nema_strlcpy(s_name, name, sizeof(s_name));
    return NEMA_OK;
}

static void host_save(void) { pal_settings_save(); }

static const psys_settings_host_t HOST = {host_font_get, host_font_set, host_radio_get, host_name_get, host_name_set,
                                          host_save};

void pal_settings_bind_host(void) {
    name_load();
    psys_settings_bind(&HOST);
}

/* ---- boot target: appended accessors ---- */

/* boot.target is a settings key (settings_kv.c): desktop or console, checked by the table. */
static void take_boot(const char *k, const char *v) { psys_settings_kv_load(k, v); }

void pal_settings_load_boot(void) { for_each_setting(take_boot); }
const char *pal_settings_boot_target(void) {
    const char *v = psys_settings_text("boot.target");
    return v[0] ? v : "desktop";
}

/* Accepts desktop or console only; anything else is refused and nothing changes. */
bool pal_settings_set_boot_target(const char *value) { return psys_settings_set("boot.target", value) == NEMA_OK; }
