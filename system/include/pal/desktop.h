/* PalanuOS desktop: the compositor.
 *
 * Layers, bottom to top: the front view (an app, or the home view), the status bar, the overlay stack (dialogs, switcher,
 * control center, lock screen), the toast layer, and the panic screen (not here yet). One framebuffer. The desktop owns
 * the status bar, the global keys (tap the cycle key to switch app, hold it for home), the redraw scheduling, the list of
 * views, the overlay stack (pal/overlay.h), the lock state, and the status item registry below. Built-in graphical apps are
 * views that run inside the desktop task, which costs no stack per app. Installed apps will get a surface of their own.
 *
 * The contract for the screens and the apps is in docs/DESKTOP.md.
 */
#ifndef PAL_DESKTOP_H
#define PAL_DESKTOP_H

#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/legend.h"
#include "pal/theme.h"

#define PAL_VIEW_FULLSCREEN 0x1u /* no status bar under this view */
#define PAL_VIEW_HIDDEN 0x2u     /* not listed in the launcher */
#define PAL_VIEW_OVERLAY 0x4u    /* as an overlay: drawn on top of the layer below, which stays visible (dialogs) */
#define PAL_VIEW_OPAQUE 0x8u     /* as an overlay: covers everything below; those layers are not drawn while it is on top */
#define PAL_VIEW_NOBAR 0x10u     /* no status bar while this view is the top layer; an overlay gets the whole screen */
#define PAL_VIEW_NOLEGEND 0x20u  /* no legend bar while this view is the top layer, or the layer the bar would show for */

typedef struct pal_view pal_view_t;
struct pal_shell_view;
struct pal_view {
    const char *id;
    const char *title;
    uint32_t flags;
    void (*enter)(pal_view_t *v);
    void (*leave)(pal_view_t *v);
    /* Draw into area. full is true when the area was just cleared and everything must be drawn. */
    void (*draw)(pal_view_t *v, pal_canvas_t *c, pal_rect_t area, bool full);
    bool (*key)(pal_view_t *v, const pal_key_t *k); /* true if handled */
    void (*pump)(pal_view_t *v);                    /* background I/O; called for every view every loop */
    void (*tick)(pal_view_t *v, uint32_t now_ms);   /* called every tick_ms while it is the top layer */
    uint32_t tick_ms;
    void *data;
    /* Appended later; optional. obscured: an overlay was pushed on top of v. exposed: the overlay above v closed. */
    void (*obscured)(pal_view_t *v);
    void (*exposed)(pal_view_t *v);
    /* Appended later; optional. The legend bar (pal/legend.h) shows these while this view is in front, or while it is an
     * overlay on top of it that has none of its own (a dialog). A static array, or the set made by pal_legend_set; the array
     * must outlive the view's use of it. NULL and 0: no legends. */
    const pal_legend_item_t *legends;
    int nlegends;
    /* Appended later; optional. A shell service (a dialog, a picker, a text entry, the busy box, the on-screen keyboard) describes
     * its state as data, so a session can draw it in its own look (pal/shell.h). */
    bool (*describe)(pal_view_t *v, struct pal_shell_view *out);
};

typedef struct {
    pal_status_t (*status)(void); /* NULL: clock only */
    const char *start_view; /* the view in front at start (boot.target=console: "terminal"); NULL: the home view */
} pal_desktop_cfg_t;

void pal_desktop_init(const pal_desktop_cfg_t *cfg); /* call first; views register after this */
nema_err_t pal_desktop_start(void);                   /* then start the task */
nema_err_t pal_desktop_register(pal_view_t *v);
/* Open a registered view as the front layer (overlays are closed first). From any task: when the desktop runs, the change
 * is queued for the desktop task and happens before the next frame. */
void pal_desktop_open(const char *id);
void pal_desktop_home(void);                       /* opens the running session's home view; nothing without a session */
void pal_app_exit(void);                           /* the app in front is done: the session decides where to go */
void pal_desktop_set_home(const char *id);         /* set by the session at its start; none without a session */
const char *pal_desktop_home_id(void);
void pal_desktop_invalidate(void);              /* ask for a redraw of the top layer; any task */
void pal_desktop_invalidate_all(void);          /* also redraw status bar and background; any task */
nema_err_t pal_desktop_watch(nema_obj_t *o);    /* wake the desktop when o becomes readable */
void pal_desktop_unwatch(nema_obj_t *o);
const char *pal_desktop_front_id(void);         /* the front view (not the overlays) */
/* Frames drawn since boot (a frame is a render that drew the front view). The OTA health check reads it. */
uint32_t pal_desktop_frames(void);
pal_view_t *pal_desktop_view(const char *id);
size_t pal_desktop_view_count(void);
pal_view_t *pal_desktop_view_at(size_t i);

/* The area available to a view: below the status bar unless it is FULLSCREEN or NOBAR, and above the legend bar while one
 * shows (a view with no legends gets the rest of the screen). */
pal_rect_t pal_desktop_area(const pal_view_t *v);

/* Lock. The lock view is registered once; locking pushes it as an opaque overlay and routes every key to it
 * until it calls pal_desktop_unlock(). Locking fails with NEMA_ERR_NOENT when no lock view is registered. */
void pal_desktop_set_lock_view(const char *view_id);
nema_err_t pal_desktop_lock(void);
void pal_desktop_unlock(void);
bool pal_desktop_locked(void);
/* Any activity from outside the keyboard (Link input, remote keys): resets the idle timers. It never wakes a sleeping display;
 * a key press does. Safe from any task. */
void pal_desktop_activity(void);

/* Status items. Owners register an item once; the desktop draws the registered items to the right of the clock, in
 * registration order, from the clock's right edge (KUI_STATUS_ITEMS_X logical pixels, scaled with the UI), and calls draw on
 * every status redraw. pal_status_changed() asks for a
 * redraw of the status bar when an owner's state moved (any task). At most 12 items. */
typedef struct pal_status_item {
    const char *id;
    /* Draw at x, h pixels high (the status bar height). Returns the width drawn, 0 when the item is not shown now. */
    int (*draw)(pal_canvas_t *c, const pal_theme_t *t, int x, int h, const pal_status_t *st);
} pal_status_item_t;
nema_err_t pal_status_item_register(const pal_status_item_t *item);
void pal_status_changed(void);

/* Legend bar (pal/legend.h). pal_legend_set makes items the legends of the view the desktop is running now (its draw,
 * key, tick, pump, enter or leave), not the one in front. A different array (or count) replaces them; the same pair again is a
 * no-op. pal_legend_clear removes them. Desktop task only; other calls do nothing. A change repaints the screen and resizes
 * the content area. */
void pal_legend_set(const pal_legend_item_t *items, int n);
void pal_legend_clear(void);
/* Legends are drawn on the home screen only (the default); false draws them wherever a view declares them. Any task. */
void pal_legend_home_only(bool on);

/* ---- Desktop chrome (appended later; docs/DESKTOPS.md) ----
 * The compositor above runs views and overlays. What surrounds them belongs to the active desktop environment (pal/desktops.h): the
 * status bar, the global keys, and whether the desktop draws the screen itself. No chrome (NULL) is the classic behaviour: the
 * themed status bar, the legend bar, and the global keys (Fn twice: input menu; cycle tap: switcher; cycle hold: home). */
typedef struct {
    bool hide_bar;    /* the compositor draws no status bar */
    bool hide_legend; /* nor the legend bar */
    int top_px;       /* rows at the top the desktop keeps for its own bar: views start below them and the compositor never sends them */
    /* Sees every key after the power and lock checks, before the compositor's global keys; true when the desktop consumed it. NULL: none. */
    bool (*key)(const pal_key_t *k);
    /* True while the desktop presents the whole screen itself (pal_display_present_rgb565): the compositor draws and sends nothing,
     * and redraws everything once it is false again. NULL: never. */
    bool (*owns_screen)(void);
    /* Called on the desktop task once per loop, after the compositor sent its frame. NULL: none. */
    void (*frame)(uint32_t now_ms);
    /* Milliseconds until frame wants to run again, or -1 for no deadline. NULL: no deadline. */
    int32_t (*next_ms)(uint32_t now_ms);
    /* The session draws the shell services and the toasts itself (pal/shell.h): the compositor draws neither. */
    bool services;
} pal_desktop_chrome_t;

/* Set by a desktop environment's start (NULL for classic); applied on the desktop task before the next frame, with a full redraw. The
 * structure must outlive its use. */
void pal_desktop_set_chrome(const pal_desktop_chrome_t *chrome);
/* The status the bar shows (clock, battery, radios, link): for a desktop that draws its own bar. The compositor's once-a-second
 * reading, with the clock current: calling it does not sample the battery. Any task. */
pal_status_t pal_desktop_status(void);

#endif
