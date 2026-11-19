/* Settings with no owning module (Settings app): a bool, an integer, a named choice, or a short text, kept in
 * /data/settings as key=value with the others. settings_api.c asks here first for any key these answer, so the table in
 * settings_api.c stays about the keys that have a module. Choices are stored and saved by their name ("saver", not 2), so
 * the file reads by hand. Effects run after a set and at boot (psys_settings_kv_apply). Readers call psys_settings_int or
 * psys_settings_text; a module that uses one of these values reads it at the point it needs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/log.h"
#include "nema/types.h"
#include "pal/trust.h"
#include "pal/desktop.h"
#include "pal/input.h"
#include "pal/kui.h"
#include "pal/power.h"
#include "pal/settings.h"

#define TEXT_MAX 24 /* profile.user: characters, NUL included */

typedef enum { KV_BOOL, KV_INT, KV_CHOICE, KV_TEXT } kv_kind_t;

typedef struct {
    const char *name;
    kv_kind_t kind;
    int32_t min, max;          /* KV_INT */
    const char *const *names;  /* KV_CHOICE: NULL-terminated */
    int32_t def;               /* KV_BOOL/INT/CHOICE; KV_TEXT uses text_def */
    const char *text_def;
    void (*apply)(const char *name, int32_t v); /* effect at boot and after a set; may be NULL */
} kv_def_t;

static const char *const PROFILE_NAMES[] = {"performance", "balanced", "saver", NULL};
static const char *const LOG_NAMES[] = {"error", "warn", "info", "debug", NULL};
static const char *const TARGET_NAMES[] = {"desktop", "console", NULL};
static const char *const ATHENA_THEMES[] = {"cupertino", "material", "softkey", "trackball", NULL}; /* athena_theme.c */

static void apply_repeat(const char *name, int32_t v) {
    static uint32_t delay = 420, rate = 55;
    if (!strcmp(name, "input.repeat_delay")) delay = (uint32_t)v;
    else rate = (uint32_t)v;
    pal_input_set_repeat(delay, rate);
}

static void apply_power(const char *name, int32_t v) {
    (void)name;
    (void)pal_power_apply((int)v);
}

static void apply_log(const char *name, int32_t v) {
    (void)name;
    nema_log_set_level((nema_log_level_t)v);
}

/* ui.font: the reference font packs (Appearances > Font); the names are the packs of kui_fonts.c, in menu order. */
static const char *const FONT_NAMES[] = {"Helvetica", "Haxrcorp4089", "ProFont", NULL};
static void apply_font(const char *name, int32_t v) {
    (void)name;
    kui_font_pack_set(FONT_NAMES[v]);
}

/* display.rotation: the reference Rotation row (0, 90, 180, 270 degrees). Stored only; the panel driver does not turn yet. */
static const char *const ROTATION_NAMES[] = {"0", "90", "180", "270", NULL};

/* ui.assets: the reference asset packs (Appearances > Asset Pack); PalanuOS ships one, so the row stores the name. */
static const char *const ASSET_NAMES[] = {"palanu", NULL};

static const kv_def_t DEFS[] = {
    {"ui.font", KV_CHOICE, 0, 5, FONT_NAMES, 0, NULL, apply_font},
    {"ui.assets", KV_CHOICE, 0, 0, ASSET_NAMES, 0, NULL, NULL},
    {"display.rotation", KV_CHOICE, 0, 3, ROTATION_NAMES, 0, NULL, NULL},
    {"ui.statusbar", KV_BOOL, 0, 1, NULL, 1, NULL, NULL},
    {"ui.legend", KV_BOOL, 0, 1, NULL, 1, NULL, NULL}, /* the legend bar at the bottom (pal/legend.h); off hides it everywhere */
    {"ui.clock24", KV_BOOL, 0, 1, NULL, 1, NULL, NULL}, /* 24 hours: what every clock shows today */
    {"notify.dnd", KV_BOOL, 0, 1, NULL, 0, NULL, NULL},
    {"notify.beep", KV_BOOL, 0, 1, NULL, 1, NULL, NULL},
    {"input.repeat_delay", KV_INT, 120, 1000, NULL, 420, NULL, apply_repeat},
    {"input.repeat_rate", KV_INT, 20, 200, NULL, 55, NULL, apply_repeat},
    {"power.profile", KV_CHOICE, 0, 2, PROFILE_NAMES, 1, NULL, apply_power}, /* applied at once and at boot (pal/power.h) */
    {"dev.mode", KV_BOOL, 0, 1, NULL, PSYS_DEV_MODE_DEFAULT, NULL, NULL}, /* the default trust.c reads too (trust.h) */
    {"log.level", KV_CHOICE, 0, 3, LOG_NAMES, 2, NULL, apply_log},
    {"boot.target", KV_CHOICE, 0, 1, TARGET_NAMES, 0, NULL, NULL},
    {"boot.quiet", KV_BOOL, 0, 1, NULL, 1, NULL, NULL}, /* the system console's splash covers the boot log until a key (console.c) */
    {"profile.user", KV_TEXT, 0, TEXT_MAX - 1, NULL, 0, "", NULL},
    {"athena.seconds", KV_BOOL, 0, 1, NULL, 1, NULL, NULL}, /* the analog clock widget's second hand (off in the golden images) */
    {"athena.theme", KV_CHOICE, 0, 3, ATHENA_THEMES, 0, NULL, NULL}, /* the Athena session's look (athena_theme.c) */
    {"desktop.env", KV_TEXT, 0, TEXT_MAX - 1, NULL, 0, "", NULL}, /* the active desktop environment (pal/desktops.h) */
};
#define NDEFS ((int)(sizeof(DEFS) / sizeof(DEFS[0])))

static int32_t s_val[NDEFS];
static char s_text[NDEFS][TEXT_MAX] NEMA_EXT_BSS; /* PSRAM: read on a settings change, not on a hot path */

static void reset_defaults(void) {
    for (int i = 0; i < NDEFS; i++) {
        s_val[i] = DEFS[i].def;
        snprintf(s_text[i], TEXT_MAX, "%s", DEFS[i].text_def ? DEFS[i].text_def : "");
    }
}

static int find(const char *key) {
    for (int i = 0; i < NDEFS; i++)
        if (!strcmp(DEFS[i].name, key)) return i;
    return -1;
}

static bool first_run = true;
static void ensure(void) {
    if (!first_run) return;
    first_run = false;
    reset_defaults();
}

bool psys_settings_kv_has(const char *key) { return key && find(key) >= 0; }

/* A strict integer within [min, max]: optional minus and digits only. */
static bool parse_in(const char *s, int32_t min, int32_t max, int32_t *out) {
    int neg = s[0] == '-';
    const char *p = neg ? s + 1 : s;
    if (!*p || strlen(p) > 6) return false;
    int32_t v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
    }
    if (neg) v = -v;
    if (v < min || v > max) return false;
    *out = v;
    return true;
}

/* A choice's index from its name, or from its index written as digits. */
static bool choice_of(const kv_def_t *d, const char *text, int32_t *out) {
    for (int i = 0; d->names[i]; i++)
        if (!strcmp(d->names[i], text)) {
            *out = i;
            return true;
        }
    return parse_in(text, d->min, d->max, out);
}

static bool text_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x20 || s[i] > 0x7E || s[i] == '=') return false;
    return true;
}

int psys_settings_kv_get(const char *key, char *out, size_t cap) {
    ensure();
    int i = key ? find(key) : -1;
    if (i < 0 || !out) return NEMA_ERR_NOENT;
    const kv_def_t *d = &DEFS[i];
    const char *text;
    char num[16];
    switch (d->kind) {
    case KV_BOOL:
    case KV_INT:
        snprintf(num, sizeof(num), "%d", (int)s_val[i]);
        text = num;
        break;
    case KV_CHOICE:
        text = d->names[s_val[i]];
        break;
    default:
        text = s_text[i];
        break;
    }
    size_t n = strlen(text);
    if (n + 1 > cap) return NEMA_ERR_RANGE;
    memcpy(out, text, n + 1);
    return (int)n;
}

/* The one place a value is checked, stored, and applied (the saver and the boot loader use the lenient form below). */
int psys_settings_kv_set(const char *key, const char *value) {
    ensure();
    int i = key && value ? find(key) : -1;
    if (i < 0) return NEMA_ERR_NOENT;
    const kv_def_t *d = &DEFS[i];
    int32_t v = 0;
    switch (d->kind) {
    case KV_BOOL:
        if (!strcmp(value, "0")) v = 0;
        else if (!strcmp(value, "1")) v = 1;
        else return NEMA_ERR_INVAL;
        break;
    case KV_INT:
        if (!parse_in(value, d->min, d->max, &v)) return NEMA_ERR_INVAL;
        break;
    case KV_CHOICE:
        if (!choice_of(d, value, &v)) return NEMA_ERR_INVAL;
        break;
    case KV_TEXT:
        if (!text_ok(value, TEXT_MAX - 1)) return NEMA_ERR_INVAL;
        snprintf(s_text[i], TEXT_MAX, "%s", value);
        pal_desktop_invalidate_all();
        return NEMA_OK;
    }
    s_val[i] = v;
    if (d->apply) d->apply(d->name, v);
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

int32_t psys_settings_int(const char *key, int32_t def) {
    ensure();
    int i = key ? find(key) : -1;
    return i >= 0 && DEFS[i].kind != KV_TEXT ? s_val[i] : def;
}

const char *psys_settings_text(const char *key) {
    ensure();
    int i = key ? find(key) : -1;
    if (i < 0) return "";
    if (DEFS[i].kind == KV_CHOICE) return DEFS[i].names[s_val[i]];
    return DEFS[i].kind == KV_TEXT ? s_text[i] : "";
}

/* Boot: one line of the settings file. Unknown or bad values keep the default; nothing is applied yet. */
void psys_settings_kv_load(const char *key, const char *value) {
    ensure();
    int i = key ? find(key) : -1;
    if (i < 0 || !value) return;
    const kv_def_t *d = &DEFS[i];
    int32_t v;
    switch (d->kind) {
    case KV_BOOL:
        if (!strcmp(value, "0")) s_val[i] = 0;
        else if (!strcmp(value, "1")) s_val[i] = 1;
        break;
    case KV_INT:
        if (parse_in(value, d->min, d->max, &v)) s_val[i] = v;
        break;
    case KV_CHOICE:
        if (choice_of(d, value, &v)) s_val[i] = v;
        break;
    case KV_TEXT:
        if (text_ok(value, TEXT_MAX - 1)) snprintf(s_text[i], TEXT_MAX, "%s", value);
        break;
    }
}

/* Boot, after the load: the effects of the loaded values (the desktop is not up yet; the effects only set state). */
void psys_settings_kv_apply(void) {
    ensure();
    for (int i = 0; i < NDEFS; i++)
        if (DEFS[i].apply) DEFS[i].apply(DEFS[i].name, s_val[i]);
}

/* The lines the saver writes: "key=value\n" for every key, in table order. Returns the length, or 0 when cap is too small. */
size_t psys_settings_kv_format(char *out, size_t cap) {
    ensure();
    size_t used = 0;
    for (int i = 0; i < NDEFS; i++) {
        char line[TEXT_MAX + 48];
        const kv_def_t *d = &DEFS[i];
        int n = 0;
        switch (d->kind) {
        case KV_BOOL:
        case KV_INT:
            n = snprintf(line, sizeof(line), "%s=%d\n", d->name, (int)s_val[i]);
            break;
        case KV_CHOICE:
            n = snprintf(line, sizeof(line), "%s=%s\n", d->name, d->names[s_val[i]]);
            break;
        case KV_TEXT:
            n = snprintf(line, sizeof(line), "%s=%s\n", d->name, s_text[i]);
            break;
        }
        if (n < 0 || used + (size_t)n + 1 > cap) return 0;
        memcpy(out + used, line, (size_t)n);
        used += (size_t)n;
    }
    out[used] = 0;
    return used;
}
