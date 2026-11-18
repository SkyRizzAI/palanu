/* PalanuOS screens: the home screen, the lock screen, the launcher skins, the control center and the
 * notification center, plus the drawing helpers they share. The screens are views (pal_view_t); pal_ui_register_all()
 * registers them at boot. docs/UI.md explains each screen, the settings keys, and how to add a skin or a wallpaper.
 *
 * Everything here draws through the active theme's palette slots (pal/theme.h), so dark mode and the other themes apply
 * without any change in the screens. */
#ifndef PAL_UI_H
#define PAL_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal/desktop.h"
#include "pal/font.h"
#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/registry.h"
#include "pal/time.h"

/* ---- settings (ui_prefs.c): ui.launcher, ui.wallpaper and security.pin ---- */
/* The four launcher skins (ui_launcher.c), in the order the Settings page lists them. */
#define PAL_UI_LAUNCHER_STRIP 0 /* Strip carousel */
#define PAL_UI_LAUNCHER_GRID 1     /* Channel grid */
#define PAL_UI_LAUNCHER_COMPACT 2 /* 3D card stack */
#define PAL_UI_LAUNCHER_LIST 3 /* list skin */
#define PAL_UI_WALL_NONE 0
#define PAL_UI_WALL_STARS 1
#define PAL_UI_WALL_CITY 2
#define PAL_UI_WALL_WAVES 3
#define PAL_UI_WALL_ANIM 4 /* anim:NAME (ui.wallpaper), the animation wallpaper: its name is pal_ui_wall_anim() */
#define PAL_UI_PIN_MIN 4
#define PAL_UI_PIN_MAX 8

void pal_ui_prefs_load(void);     /* reads /data/ui once at boot; a missing file keeps the defaults */
/* UI scale (ui.scale, pal/kui.h): the choices Settings offers, in quarters, and their names ("1x" .. "3x"). Index -1 or a
 * missing value is the board's default (pal_board_desc_t.ui_scale_q). Setting it applies at once (kui_set_scale_q) and
 * repaints every screen. */
#define PAL_UI_SCALE_COUNT 7
extern const int PAL_UI_SCALE_Q[PAL_UI_SCALE_COUNT];
extern const char *const PAL_UI_SCALE_NAMES[PAL_UI_SCALE_COUNT];
int pal_ui_scale(void);           /* index into PAL_UI_SCALE_Q of the scale in use */
bool pal_ui_set_scale(int index);
int pal_ui_scale_default(void);   /* index of the board's default */
int pal_ui_launcher(void);        /* PAL_UI_LAUNCHER_* */
bool pal_ui_set_launcher(int style);
int pal_ui_wallpaper(void);       /* PAL_UI_WALL_* */
bool pal_ui_set_wallpaper(int wall);
const char *pal_ui_launcher_name(int style);  /* "strip" | "grid" | "compact" | "list" */
void pal_ui_launcher_fixture(bool on);        /* uishow launcher SKIN ref: the reference entries (launcher_fixture.c) */
bool pal_launcher_pinned(const char *id);     /* the launcher shows this app (launcher.c); the others are in the app list */
void pal_launcher_run(const pal_reg_app_t *e); /* opens a launcher or app list entry (launcher.c) */
void pal_ui_applist_fixture(bool on);         /* uishow applist ref: the reference app list (applist.c) */
bool pal_ui_ref_fixture(const char *name);  /* uishow screen NAME: the reference dialog or page (dialog_fixture.c) */
extern pal_view_t pal_launcher_view;           /* id "launcher": the classic launcher (launcher.c) */
extern pal_view_t pal_applist_view;           /* id "applist": the app list, opened from the launcher's Apps entry */
const char *pal_ui_wallpaper_name(int wall);  /* "anim:NAME" for the animation wallpaper (the desktop's only one) */
/* The wallpapers ui.wallpaper offers: the desktop animations (the reference laptop, hacking_pc and lab_research, drawn for PalanuOS).
 * Old values (none, stars, city, waves, anim:NAME of another name) load as the first one. */
#define PAL_UI_WALL_COUNT 3
extern const char *const PAL_UI_WALL_NAMES[PAL_UI_WALL_COUNT];
bool pal_ui_set_wallpaper_anim(const char *name); /* false unless the animation exists */
const char *pal_ui_wall_anim(void);           /* the animation name for the animation wallpaper */
const char *pal_ui_lock_anim(void);           /* ui.lock_anim: "none" or an animation name (default sleepy-cat) */
bool pal_ui_set_lock_anim(const char *name);
int pal_ui_wall_fit(void);                    /* ui.wall_fit: 0 center, 1 fit, 2 stretch, 3 crop */
bool pal_ui_set_wall_fit(int fit);
const char *pal_ui_wall_fit_name(int fit);
int pal_ui_wall_anchor(void);                 /* ui.wall_anchor: 0..8, the nine anchors in reading order */
bool pal_ui_set_wall_anchor(int anchor);
const char *pal_ui_wall_anchor_name(int anchor);
bool pal_ui_pin_set(const char *pin);         /* "" clears; 4 to 8 digits sets; false for anything else */
bool pal_ui_pin_is_set(void);
bool pal_ui_pin_check(const char *pin);       /* true when the PIN is the stored one (false when none is set) */
const char *pal_ui_device_name(void);         /* device.name, for the launcher title */

/* ---- formatting (ui_fmt.c; pure) ---- */
void pal_ui_fmt_hm(int hour, int minute, char *out, size_t cap);                 /* "10:09" */
void pal_ui_fmt_date(const pal_tm_t *tm, char *out, size_t cap);                 /* "Sun 1 Feb 2026" */
/* A notification's age: "now" under a minute, "12m" under an hour, "10:04" the same local day, "2d" otherwise. */
void pal_ui_fmt_age(uint32_t now, uint32_t then, int tz_minutes, char *out, size_t cap);

/* ---- layout (ui_layout.c; pure) ---- */
/* Scroll so that cursor is visible; returns the first visible index for a window of visible items. */
int pal_ui_window_first(int n, int cursor, int visible);

/* ---- lock PIN entry (ui_pin.c; pure state machine) ---- */
typedef struct {
    char digits[PAL_UI_PIN_MAX + 1];
    int len;
    int wrong;       /* failed attempts since the last success */
    bool last_wrong; /* the last submit failed (shows the message until the next key) */
} pal_ui_pin_t;

typedef enum {
    PAL_UI_PIN_NONE,    /* the key did nothing visible */
    PAL_UI_PIN_EDITED,  /* a digit or backspace changed the entry */
    PAL_UI_PIN_OK,      /* submit matched */
    PAL_UI_PIN_WRONG,   /* submit did not match; the entry is cleared */
    PAL_UI_PIN_SHORT,   /* submit with fewer than PAL_UI_PIN_MIN digits */
} pal_ui_pin_event_t;

void pal_ui_pin_reset(pal_ui_pin_t *p);
pal_ui_pin_event_t pal_ui_pin_key(pal_ui_pin_t *p, const pal_key_t *k, bool (*check)(const char *));

/* ---- drawing helpers (ui_common.c). The key legends at the bottom are the legend bar now (pal/legend.h). ---- */
/* Truncates text with ".." to fit maxw pixels in font f into out. Returns the width of out. */
int pal_ui_text_fit(const pal_font_t *f, const char *text, int maxw, char *out, size_t cap);
/* Text drawn with each glyph pixel enlarged scale times. Returns the x after the text. */
int pal_ui_big_text(pal_canvas_t *c, const pal_font_t *f, int x, int y, const char *text, int scale, pal_color_t fg);
/* A rounded box filled with fill and outlined 1 px in edge. r: corner radius. */
void pal_ui_box(pal_canvas_t *c, int x, int y, int w, int h, int r, pal_color_t fill, pal_color_t edge);
/* A status chip: a rounded box with an 8x8 icon and a caption. Returns its width. on = inverted look. */
int pal_ui_chip(pal_canvas_t *c, const pal_theme_t *t, int x, int y, const char *icon, const char *label, bool on);
/* A vertical pill slider with the value 0..max drawn from the bottom. enabled false draws it dimmed. */
void pal_ui_vslider(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int w, int h, int value, int max, bool focused,
                    bool enabled);
void pal_ui_scrollbar(pal_canvas_t *c, int x, int y, int h, int offset, int visible, int total);

/* ---- screens ---- */
extern pal_view_t pal_home_view;
extern pal_view_t pal_lock_view;
extern pal_view_t pal_control_view;
extern pal_view_t pal_notify_view;
extern pal_view_t pal_gallery_view; /* dev: every icon at its sizes (uishow gallery) */

/* Registers the screens (home, lock, control, notifications) and the launcher skin settings; boot calls it. */
void pal_ui_register_all(void);
/* Show the lock screen now (the control center's Lock tile and the lock key), and leave it (PIN or two Enters). */
void pal_ui_lock_now(void);
void pal_ui_unlock(void);
/* Ask the desktop to open a view from any task (uishow, the golden tool). Taken up on the desktop's next pump. */
void pal_ui_request_view(const char *id);
/* The golden tool freezes the wallpaper at a frame so screens are reproducible. frame < 0 unfreezes. */
void pal_ui_home_freeze(int frame);
/* The golden tool freezes the lock screen's animation at a playback position (frame < 0 unfreezes). */
void pal_ui_lock_freeze(int frame);
/* Animation screens (ui_splash.c, ui_anim_view.c). The splash fills its bar over 2 s (the reference); ms >= 0 holds the bar at that many
 * milliseconds (the golden tool). pal_ui_splash_restart shows the restart splash and then restarts the board. */
void pal_ui_splash_freeze(int ms);
void pal_ui_splash_show(void);    /* the classic splash as an opaque overlay (uishow; the boot shows the system console's splash) */
void pal_ui_splash_restart(void);
/* anim play NAME: the animation full screen at scale 1..3; ENTER plays its active sequence, ESC closes. anim sheet: every frame of
 * an animation, six to a page. Both open from any task. */
void pal_ui_anim_play(const char *name, int scale);
void pal_ui_anim_sheet(const char *name, int page);
extern pal_view_t pal_splash_view;
extern pal_view_t pal_animplay_view;
extern pal_view_t pal_animsheet_view;

#endif
