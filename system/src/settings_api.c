/* The system settings table. Each key is one row: its name, its kind, the range of its values, whether it is read
 * only, a getter (text) and a setter (applies a checked value). Adding a setting is one row here. The setters apply at once
 * through the system's own functions (theme, display, time, desktop); the values that the built-in apps own go through
 * the host bound by psys_settings_bind. Every change is saved and then reported to the change callback. */
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "nema/log.h"
#include "nema/types.h"
#include "pal/anim.h"
#include "pal/desktop.h"
#include "pal/dpm.h"
#include "pal/link.h"
#include "pal/perf.h"
#include "pal/display.h"
#include "pal/settings.h"
#include "pal/theme.h"
#include "pal/time.h"
#include "pal/ui.h"
#include "settings_api.h"

#define VAL_MAX 32     /* the longest text a getter writes, NUL included */
#define NAME_MAX_LEN 31 /* device.name: characters */

typedef enum { K_BOOL, K_INT, K_THEME, K_TEXT, K_CHOICE, K_PIN, K_ANIM } kind_t; /* K_ANIM: "none" or an animation name */

typedef struct {
    const char *name;
    kind_t kind;
    int32_t min, max;
    int read_only;
    int (*get)(char *out, size_t cap); /* text into out; returns its length or a negative error */
    int (*set)(const char *text, int32_t v); /* applies a value the table has checked (v from K_BOOL, K_INT, K_THEME, K_CHOICE) */
} row_t;

static psys_settings_host_t s_host;
static int s_bound;
static psys_settings_cb_t s_cb;

static int put(char *out, size_t cap, const char *text) {
    size_t n = strlen(text);
    if (n + 1 > cap) return NEMA_ERR_RANGE;
    memcpy(out, text, n + 1);
    return (int)n;
}

static int put_int(char *out, size_t cap, int32_t v) {
    char t[16];
    snprintf(t, sizeof(t), "%d", (int)v);
    return put(out, cap, t);
}

/* A strict integer: an optional minus sign and decimal digits, nothing else, within [min, max]. */
static int parse_int(const char *s, int32_t min, int32_t max, int32_t *out) {
    int neg = s[0] == '-';
    const char *p = neg ? s + 1 : s;
    if (!*p || strlen(p) > 6) return 0;
    int32_t v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
    }
    if (neg) v = -v;
    if (v < min || v > max) return 0;
    *out = v;
    return 1;
}

static void apply_theme(void) {
    pal_display_refresh_theme();
    pal_desktop_invalidate_all();
}

/* ---- ui ---- */

static int g_dark(char *o, size_t c) { return put_int(o, c, pal_theme_dark() ? 1 : 0); }
static int s_dark(const char *t, int32_t v) {
    (void)t;
    pal_theme_select(pal_theme_index(), v != 0, pal_theme_size());
    apply_theme();
    return NEMA_OK;
}

static int g_theme(char *o, size_t c) { return put(o, c, pal_theme_name(pal_theme_index())); }
static int s_theme(const char *t, int32_t v) {
    (void)t;
    pal_theme_select(v, pal_theme_dark(), pal_theme_size());
    apply_theme();
    return NEMA_OK;
}

static int g_size(char *o, size_t c) { return put_int(o, c, pal_theme_size()); }
static int s_size(const char *t, int32_t v) {
    (void)t;
    pal_theme_select(pal_theme_index(), pal_theme_dark(), v);
    apply_theme();
    return NEMA_OK;
}

/* ---- time and display ---- */

static int g_tz(char *o, size_t c) { return put_int(o, c, pal_time_tz_minutes()); }
static int s_tz(const char *t, int32_t v) {
    (void)t;
    pal_time_set_tz_minutes(v);
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

static int g_bright(char *o, size_t c) { return put_int(o, c, pal_display_brightness()); }
static int s_bright(const char *t, int32_t v) {
    (void)t;
    pal_display_set_brightness(v);
    return NEMA_OK;
}

/* ---- values the built-in apps own ---- */

static int g_font(char *o, size_t c) {
    if (!s_host.font_get) return NEMA_ERR_NOENT;
    return put_int(o, c, s_host.font_get());
}
static int s_font(const char *t, int32_t v) {
    (void)t;
    if (!s_host.font_set) return NEMA_ERR_NOENT;
    s_host.font_set(v);
    return NEMA_OK;
}

static int g_radio_wifi(char *o, size_t c) {
    const char *v = s_host.radio_get ? s_host.radio_get("radio.wifi") : NULL;
    return v ? put(o, c, v) : NEMA_ERR_NOENT;
}
static int g_radio_ble(char *o, size_t c) {
    const char *v = s_host.radio_get ? s_host.radio_get("radio.ble") : NULL;
    return v ? put(o, c, v) : NEMA_ERR_NOENT;
}

static int g_name(char *o, size_t c) {
    const char *v = s_host.name_get ? s_host.name_get() : NULL;
    return v ? put(o, c, v) : NEMA_ERR_NOENT;
}
static int s_name(const char *t, int32_t v) {
    (void)v;
    if (!s_host.name_set) return NEMA_ERR_NOENT;
    return s_host.name_set(t);
}

/* ---- display power: seconds in the table, milliseconds in the power manager ---- */

static int g_sleep(char *o, size_t c) { return put_int(o, c, (int32_t)(pal_dpm_sleep_ms() / 1000u)); }
static int s_sleep(const char *t, int32_t v) {
    (void)t;
    pal_dpm_set_sleep_ms((uint32_t)v * 1000u);
    return NEMA_OK;
}
static int g_lock(char *o, size_t c) { return put_int(o, c, (int32_t)(pal_dpm_lock_ms() / 1000u)); }
static int s_lock(const char *t, int32_t v) {
    (void)t;
    pal_dpm_set_lock_ms((uint32_t)v * 1000u);
    return NEMA_OK;
}

/* ---- ui (ui_prefs.c): the launcher skin, the wallpaper, and the lock PIN ---- */

static const char *const LAUNCHER_CHOICES[] = {"strip", "grid", "compact", "list", NULL};
static const char *const WALL_CHOICES[] = {"laptop-cat", "desk-hacker", "lab", NULL}; /* PAL_UI_WALL_NAMES */
static const char *const FIT_CHOICES[] = {"center", "fit", "stretch", "crop", NULL};
static const char *const ANCHOR_CHOICES[] = {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom",
                                             "bottom-right", NULL};

/* The allowed texts of a K_CHOICE row (NULL-terminated); v in the table is the index. */
static const char *const SCALE_CHOICES[PAL_UI_SCALE_COUNT + 1] = {"1x", "1.25x", "1.5x", "1.75x", "2x", "2.25x", "2.5x", NULL};

static const char *const *choices_of(const row_t *r) {
    if (strcmp(r->name, "ui.scale") == 0) return SCALE_CHOICES;
    if (strcmp(r->name, "ui.launcher") == 0) return LAUNCHER_CHOICES;
    if (strcmp(r->name, "ui.wall_fit") == 0) return FIT_CHOICES;
    if (strcmp(r->name, "ui.wall_anchor") == 0) return ANCHOR_CHOICES;
    return WALL_CHOICES;
}

/* ui.scale (pal/kui.h): every screen is drawn again at the new scale; areas under the bars change size with it. */
static int g_scale(char *o, size_t c) { return put(o, c, PAL_UI_SCALE_NAMES[pal_ui_scale()]); }
static int s_scale(const char *t, int32_t v) {
    (void)t;
    if (!pal_ui_set_scale(v)) return NEMA_ERR_INVAL;
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

/* ---- runtime settings of other modules: the performance overlay and the cable trust ---- */

static int g_perf(char *o, size_t c) { return put_int(o, c, pal_perf_overlay_on() ? 1 : 0); }
static int s_perf(const char *t, int32_t v) {
    (void)t;
    pal_perf_overlay_set(v != 0);
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

static int g_trust(char *o, size_t c) { return put_int(o, c, pal_link_uart_trusted() ? 1 : 0); }
static int s_trust(const char *t, int32_t v) {
    (void)t;
    return pal_link_uart_trust(v != 0);
}

static int g_launcher(char *o, size_t c) { return put(o, c, pal_ui_launcher_name(pal_ui_launcher())); }
static int s_launcher(const char *t, int32_t v) {
    (void)t;
    if (!pal_ui_set_launcher(v)) return NEMA_ERR_INVAL;
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

/* ui.wallpaper: the desktop animation's name (PAL_UI_WALL_NAMES). */
static int g_wall(char *o, size_t c) { return put(o, c, pal_ui_wall_anim()); }
static int s_wall(const char *t, int32_t v) {
    (void)t;
    if (v < 0 || v >= PAL_UI_WALL_COUNT || !pal_ui_set_wallpaper_anim(PAL_UI_WALL_NAMES[v])) return NEMA_ERR_INVAL;
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

/* ui.lock_anim: "none" or an animation name (the lock screen shows it beside the clock). */
static int g_lock_anim(char *o, size_t c) { return put(o, c, pal_ui_lock_anim()); }
static int s_lock_anim(const char *t, int32_t v) {
    (void)v;
    return pal_ui_set_lock_anim(t) ? NEMA_OK : NEMA_ERR_INVAL;
}

/* ui.wall_fit and ui.wall_anchor: how the animation wallpaper is fitted to the screen. */
static int g_fit(char *o, size_t c) { return put(o, c, pal_ui_wall_fit_name(pal_ui_wall_fit())); }
static int s_fit(const char *t, int32_t v) {
    (void)t;
    if (!pal_ui_set_wall_fit(v)) return NEMA_ERR_INVAL;
    pal_desktop_invalidate_all();
    return NEMA_OK;
}
static int g_anchor(char *o, size_t c) { return put(o, c, pal_ui_wall_anchor_name(pal_ui_wall_anchor())); }
static int s_anchor(const char *t, int32_t v) {
    (void)t;
    if (!pal_ui_set_wall_anchor(v)) return NEMA_ERR_INVAL;
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

/* The PIN itself is never read back: the getter says whether one is set. */
static int g_pin(char *o, size_t c) { return put(o, c, pal_ui_pin_is_set() ? "set" : "none"); }
static int s_pin(const char *t, int32_t v) {
    (void)v;
    return pal_ui_pin_set(t) ? NEMA_OK : NEMA_ERR_INVAL;
}

static const row_t ROWS[] = {
    {"ui.dark", K_BOOL, 0, 1, 0, g_dark, s_dark},
    {"ui.theme", K_THEME, 0, 0, 0, g_theme, s_theme},
    {"ui.size", K_INT, 0, 1, 0, g_size, s_size},
    {"time.tz", K_INT, -720, 840, 0, g_tz, s_tz},
    {"display.brightness", K_INT, 0, 100, 0, g_bright, s_bright},
    {"term.font", K_INT, 0, 2, 0, g_font, s_font},
    {"radio.wifi", K_TEXT, 0, 0, 1, g_radio_wifi, NULL},
    {"radio.ble", K_TEXT, 0, 0, 1, g_radio_ble, NULL},
    {"device.name", K_TEXT, 0, 0, 0, g_name, s_name},
    {"ui.launcher", K_CHOICE, 0, 3, 0, g_launcher, s_launcher},
    {"ui.scale", K_CHOICE, 0, PAL_UI_SCALE_COUNT - 1, 0, g_scale, s_scale},
    {"ui.wallpaper", K_CHOICE, 0, PAL_UI_WALL_COUNT - 1, 0, g_wall, s_wall}, /* PAL_UI_WALL_NAMES; old values map to the first */
    {"ui.lock_anim", K_ANIM, 0, 0, 0, g_lock_anim, s_lock_anim},
    {"ui.wall_fit", K_CHOICE, 0, 3, 0, g_fit, s_fit},
    {"ui.wall_anchor", K_CHOICE, 0, 8, 0, g_anchor, s_anchor},
    {"security.pin", K_PIN, 0, 0, 0, g_pin, s_pin},
    {"perf.overlay", K_BOOL, 0, 1, 0, g_perf, s_perf},
    {"link.uart.trust", K_BOOL, 0, 1, 0, g_trust, s_trust},
    {"display.sleep", K_INT, 0, 3600, 0, g_sleep, s_sleep},
    {"display.lock", K_INT, 0, 3600, 0, g_lock, s_lock},
};

static const row_t *find(const char *key) {
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++)
        if (strcmp(ROWS[i].name, key) == 0) return &ROWS[i];
    return NULL;
}

static int name_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; }

/* Checks text against a row. On success the value is in *v (for K_THEME, a theme index). */
static int check(const row_t *r, const char *text, int32_t *v) {
    switch (r->kind) {
    case K_BOOL:
    case K_INT:
        return parse_int(text, r->min, r->max, v);
    case K_THEME: {
        if (parse_int(text, 0, pal_theme_count() - 1, v)) return 1;
        for (int i = 0; i < pal_theme_count(); i++)
            if (strcmp(pal_theme_name(i), text) == 0) {
                *v = i;
                return 1;
            }
        return 0;
    }
    case K_CHOICE:
        if (strcmp(r->name, "ui.wallpaper") == 0) { /* old values (none, stars, city, waves) mean the first wallpaper */
            const char *name = strncmp(text, "anim:", 5) == 0 ? text + 5 : text;
            for (int i = 0; i < PAL_UI_WALL_COUNT; i++)
                if (strcmp(PAL_UI_WALL_NAMES[i], name) == 0) {
                    *v = i;
                    return 1;
                }
            *v = 0;
            return strcmp(text, "none") == 0 || strcmp(text, "stars") == 0 || strcmp(text, "city") == 0 || strcmp(text, "waves") == 0;
        }
        for (int i = 0; choices_of(r)[i]; i++)
            if (strcmp(choices_of(r)[i], text) == 0) {
                *v = i;
                return 1;
            }
        return 0;
    case K_ANIM:
        if (strcmp(text, "none") == 0) return 1;
        return pal_anim_exists(text);
    case K_PIN: { /* "" clears the PIN; otherwise 4 to 8 digits */
        size_t n = strlen(text);
        if (n == 0) return 1;
        if (n < PAL_UI_PIN_MIN || n > PAL_UI_PIN_MAX) return 0;
        for (size_t i = 0; i < n; i++)
            if (text[i] < '0' || text[i] > '9') return 0;
        return 1;
    }
    case K_TEXT: {
        if (strcmp(r->name, "device.name") != 0) return 0;
        size_t n = strlen(text);
        if (n == 0 || n > NAME_MAX_LEN) return 0;
        /* device.name is shown as the launcher title (and as the BLE name): a space inside it is allowed (the reference "My Palanu"),
         * not at either end; no other consumer parses it */
        if (text[0] == ' ' || text[n - 1] == ' ') return 0;
        for (size_t i = 0; i < n; i++)
            if (!name_char(text[i]) && text[i] != ' ') return 0;
        return 1;
    }
    }
    return 0;
}

int psys_settings_get(const char *key, char *out, size_t cap) {
    if (!key || !out) return NEMA_ERR_INVAL;
    if (psys_settings_kv_has(key)) return psys_settings_kv_get(key, out, cap);
    const row_t *r = find(key);
    if (!r) return NEMA_ERR_NOENT;
    char tmp[VAL_MAX];
    int n = r->get(tmp, sizeof(tmp));
    if (n < 0) return n;
    return put(out, cap, tmp);
}

/* Keys in settings_kv.c: checked and applied there; saved and reported here like the rest. */
static int kv_set(const char *key, const char *value) {
    int rc = psys_settings_kv_set(key, value);
    if (rc != NEMA_OK) return rc;
    NEMA_LOGI("settings", "%s = %s", key, value);
    if (s_bound && s_host.save) s_host.save();
    if (s_cb) s_cb(key);
    return NEMA_OK;
}

static int settings_set(const char *key, const char *value);

/* Every change also reaches the apps that listen: a SETTING event with the key's hash (app_events.h). */
int psys_settings_set(const char *key, const char *value) {
    int rc = settings_set(key, value);
    if (rc == NEMA_OK) psys_events_post_all(7 /* PAL_EV_SETTING */, psys_events_key_hash(key), 0, 0);
    return rc;
}

static int settings_set(const char *key, const char *value) {
    if (!key || !value) return NEMA_ERR_INVAL;
    if (psys_settings_kv_has(key)) return kv_set(key, value);
    const row_t *r = find(key);
    if (!r) return NEMA_ERR_NOENT;
    if (r->read_only) return NEMA_ERR_PERM;
    int32_t v = 0;
    if (!check(r, value, &v)) return NEMA_ERR_INVAL;
    int rc = r->set(value, v);
    if (rc != NEMA_OK) return rc;
    NEMA_LOGI("settings", "%s = %s", key, value);
    if (s_bound && s_host.save) s_host.save();
    if (s_cb) s_cb(key);
    return NEMA_OK;
}

void psys_settings_bind(const psys_settings_host_t *host) {
    if (!host) {
        memset(&s_host, 0, sizeof(s_host));
        s_bound = 0;
        return;
    }
    s_host = *host;
    s_bound = 1;
}

void psys_settings_on_change(psys_settings_cb_t cb) { s_cb = cb; }
