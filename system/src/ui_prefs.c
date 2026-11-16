/* The screens' settings: ui.launcher, ui.wallpaper (including anim:NAME), ui.lock_anim, ui.wall_fit, ui.wall_anchor and security.pin.
 * Kept in /data/ui ("key=value" lines), apart from the terminal's settings file, so the screens need no change in apps/. The PIN
 * is stored as a SHA-256 digest, never as text. */
#include <stdio.h>
#include <string.h>

#include "nema/vfs.h"
#include "pal/anim.h"
#include "pal/sha256.h"
#include "pal/ui.h"
#include "pal/kui.h"
#include "pal/board.h"
#include "settings_api.h"

#define PREFS_PATH "/data/ui"

static struct {
    int launcher, wallpaper;
    char wall_anim[PAL_ANIM_NAME_MAX];  /* the animation when wallpaper is PAL_UI_WALL_ANIM */
    char lock_anim[PAL_ANIM_NAME_MAX];  /* "none" or an animation name */
    int wall_fit, wall_anchor;          /* indexes into the fit and anchor names below */
    char pin_hex[65];                   /* empty when no PIN is set */
    bool loaded;
    int scale;                          /* index into PAL_UI_SCALE_Q; -1: the board's default */
} P = {PAL_UI_LAUNCHER_STRIP, PAL_UI_WALL_ANIM, "laptop-cat", "sleepy-cat", 1, 4, "", false, -1}; /* The reference defaults: strip launcher, the laptop wallpaper, fit, centre */

/* 1x shows the most (320x240 logical, one panel pixel each), 2x is what the screens were drawn for, 2.5x the biggest:
 * below 128x96 the screens drawn for 160x120 no longer fit. The quarter steps between are uneven: some logical pixels get
 * one panel pixel more than their neighbours. */
const int PAL_UI_SCALE_Q[PAL_UI_SCALE_COUNT] = {4, 5, 6, 7, 8, 9, 10};
const char *const PAL_UI_SCALE_NAMES[PAL_UI_SCALE_COUNT] = {"1x", "1.25x", "1.5x", "1.75x", "2x", "2.25x", "2.5x"};

static const char *const LAUNCHER_NAMES[] = {"strip", "grid", "compact", "list"}; /* The reference names and order */
const char *const PAL_UI_WALL_NAMES[PAL_UI_WALL_COUNT] = {"laptop-cat", "desk-hacker", "lab"};
static const char *const FIT_NAMES[] = {"center", "fit", "stretch", "crop"};
static const char *const ANCHOR_NAMES[] = {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right"};

const char *pal_ui_launcher_name(int style) { return LAUNCHER_NAMES[style < 0 || style > 3 ? 0 : style]; }

const char *pal_ui_wallpaper_name(int wall) {
    static char anim[PAL_ANIM_NAME_MAX + 6];
    (void)wall; /* the desktop has one wallpaper kind: the animation */
    snprintf(anim, sizeof(anim), "anim:%s", P.wall_anim);
    return anim;
}

const char *pal_ui_wall_fit_name(int fit) { return FIT_NAMES[fit < 0 || fit > 3 ? 0 : fit]; }
const char *pal_ui_wall_anchor_name(int anchor) { return ANCHOR_NAMES[anchor < 0 || anchor > 8 ? 4 : anchor]; }

static void save(void) {
    char text[400];
    int n = snprintf(text, sizeof(text), "launcher=%s\nwallpaper=%s\nlock_anim=%s\nwall_fit=%s\nwall_anchor=%s\npin=%s\nscale=%s\n",
                     pal_ui_launcher_name(P.launcher), pal_ui_wallpaper_name(P.wallpaper), P.lock_anim, pal_ui_wall_fit_name(P.wall_fit),
                     pal_ui_wall_anchor_name(P.wall_anchor), P.pin_hex, P.scale >= 0 ? PAL_UI_SCALE_NAMES[P.scale] : "default");
    nema_obj_t *f;
    if (nema_fs_open(PREFS_PATH, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) == NEMA_OK) {
        nema_obj_write(f, text, (size_t)n, 0);
        nema_obj_unref(f);
    }
}

static int index_of(const char *const *names, int count, const char *v) {
    for (int i = 0; i < count; i++)
        if (!strcmp(v, names[i])) return i;
    return -1;
}

static void take(const char *k, const char *v) {
    int i;
    if (!strcmp(k, "launcher")) {
        if ((i = index_of(LAUNCHER_NAMES, 4, v)) >= 0) P.launcher = i;
        else if (!strcmp(v, "carousel")) P.launcher = PAL_UI_LAUNCHER_STRIP; /* the names before the reference skins */
        else if (!strcmp(v, "grid")) P.launcher = PAL_UI_LAUNCHER_GRID;
    } else if (!strcmp(k, "wallpaper")) {
        /* a name from PAL_UI_WALL_NAMES (or anim:NAME of one); any other saved value (none, stars, city, waves) is the default */
        const char *name = !strncmp(v, "anim:", 5) ? v + 5 : v;
        i = index_of(PAL_UI_WALL_NAMES, PAL_UI_WALL_COUNT, name);
        P.wallpaper = PAL_UI_WALL_ANIM;
        nema_strlcpy(P.wall_anim, PAL_UI_WALL_NAMES[i >= 0 ? i : 0], sizeof(P.wall_anim));
    } else if (!strcmp(k, "lock_anim")) {
        nema_strlcpy(P.lock_anim, v, sizeof(P.lock_anim));
    } else if (!strcmp(k, "wall_fit")) {
        if ((i = index_of(FIT_NAMES, 4, v)) >= 0) P.wall_fit = i;
    } else if (!strcmp(k, "wall_anchor")) {
        if ((i = index_of(ANCHOR_NAMES, 9, v)) >= 0) P.wall_anchor = i;
    } else if (!strcmp(k, "scale")) {
        P.scale = index_of(PAL_UI_SCALE_NAMES, PAL_UI_SCALE_COUNT, v); /* "default" or an unknown name: -1 */
    } else if (!strcmp(k, "pin") && strlen(v) == 64) {
        nema_strlcpy(P.pin_hex, v, sizeof(P.pin_hex));
    }
}

int pal_ui_scale_default(void) {
    const pal_board_desc_t *b = pal_board_info();
    int q = b && b->ui_scale_q ? b->ui_scale_q : 8;
    for (int i = 0; i < PAL_UI_SCALE_COUNT; i++)
        if (PAL_UI_SCALE_Q[i] == q) return i;
    return 4; /* 2x */
}

int pal_ui_scale(void) { return P.scale >= 0 ? P.scale : pal_ui_scale_default(); }

bool pal_ui_set_scale(int index) {
    if (index < 0 || index >= PAL_UI_SCALE_COUNT) return false;
    P.scale = index == pal_ui_scale_default() ? -1 : index; /* the default follows the board */
    kui_set_scale_q(PAL_UI_SCALE_Q[index]);
    save();
    return true;
}

void pal_ui_prefs_load(void) {
    if (P.loaded) return;
    P.loaded = true;
    kui_set_scale_q(PAL_UI_SCALE_Q[pal_ui_scale_default()]); /* until (and unless) the file says otherwise */
    nema_obj_t *f;
    if (nema_fs_open(PREFS_PATH, NEMA_O_READ, &f) != NEMA_OK) return;
    char buf[384];
    int n = nema_obj_read(f, buf, sizeof(buf) - 1, 0);
    nema_obj_unref(f);
    if (n <= 0) return;
    buf[n] = 0;
    char *save_p = NULL;
    for (char *line = strtok_r(buf, "\n", &save_p); line; line = strtok_r(NULL, "\n", &save_p)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        take(line, eq + 1);
    }
    kui_set_scale_q(PAL_UI_SCALE_Q[pal_ui_scale()]);
}

int pal_ui_launcher(void) { return P.launcher; }
int pal_ui_wallpaper(void) { return P.wallpaper; }

bool pal_ui_set_launcher(int style) {
    if (style < 0 || style > 3) return false;
    P.launcher = style;
    save();
    return true;
}

bool pal_ui_set_wallpaper(int wall) {
    if (wall < 0 || wall > 3) return false;
    P.wallpaper = wall;
    save();
    return true;
}

bool pal_ui_set_wallpaper_anim(const char *name) {
    if (!pal_anim_exists(name)) return false;
    nema_strlcpy(P.wall_anim, name, sizeof(P.wall_anim));
    P.wallpaper = PAL_UI_WALL_ANIM;
    save();
    return true;
}

const char *pal_ui_wall_anim(void) { return P.wall_anim; }

const char *pal_ui_lock_anim(void) { return P.lock_anim[0] ? P.lock_anim : "none"; }

bool pal_ui_set_lock_anim(const char *name) {
    if (!name) return false;
    if (strcmp(name, "none") != 0 && !pal_anim_exists(name)) return false;
    nema_strlcpy(P.lock_anim, strcmp(name, "none") == 0 ? "" : name, sizeof(P.lock_anim));
    save();
    return true;
}

int pal_ui_wall_fit(void) { return P.wall_fit; }
bool pal_ui_set_wall_fit(int fit) {
    if (fit < 0 || fit > 3) return false;
    P.wall_fit = fit;
    save();
    return true;
}

int pal_ui_wall_anchor(void) { return P.wall_anchor; }
bool pal_ui_set_wall_anchor(int anchor) {
    if (anchor < 0 || anchor > 8) return false;
    P.wall_anchor = anchor;
    save();
    return true;
}

static void digest(const char *pin, char out[65]) {
    pal_sha256_t c;
    uint8_t d[32];
    pal_sha256_init(&c);
    pal_sha256_update(&c, "palanu-pin:", 11);
    pal_sha256_update(&c, pin, strlen(pin));
    pal_sha256_final(&c, d);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

bool pal_ui_pin_set(const char *pin) {
    size_t n = strlen(pin);
    if (n == 0) {
        P.pin_hex[0] = 0;
        save();
        return true;
    }
    if (n < PAL_UI_PIN_MIN || n > PAL_UI_PIN_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (pin[i] < '0' || pin[i] > '9') return false;
    digest(pin, P.pin_hex);
    save();
    return true;
}

bool pal_ui_pin_is_set(void) { return P.pin_hex[0] != 0; }

bool pal_ui_pin_check(const char *pin) {
    if (!P.pin_hex[0]) return false;
    char h[65];
    digest(pin, h);
    return strcmp(h, P.pin_hex) == 0;
}

const char *pal_ui_device_name(void) {
    static char name[32];
    if (psys_settings_get("device.name", name, sizeof(name)) < 0) return "PalanuOS";
    return name;
}
