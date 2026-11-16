/* The boot splash and the restart splash (the reference SplashScreen, kui_splash.c). The logo sits over a progress bar that fills in 2 s.
 * The boot splash is an overlay over the desktop, shown at start (boot.c): it goes when the 2 s have passed and every boot stage
 * is recorded, or after SPLASH_MAX_MS whatever the stages say. The restart splash is the same screen: it restarts the board
 * when the 2 s are up. Keys are ignored while the splash is up. */
#include <string.h>

#include "nema/types.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/kui.h"
#include "pal/overlay.h"
#include "pal/perf.h"
#include "pal/ui.h"

#define SPLASH_MS 2000u     /* The reference kSplashMs: the progress bar fills over this long */
#define SPLASH_MAX_MS 5000u /* the boot splash never waits longer than this for the stages */
#define SPLASH_TOTAL 6      /* boot stages: start, kernel, storage, display, desktop, services */
#define SPLASH_TICK_MS 100

static struct {
    bool restart;   /* the restart splash: the board restarts when the bar is full */
    bool started;
    uint32_t t0;    /* when the splash came up (its own clock, from the first tick) */
    int freeze;     /* >= 0: the golden tool holds the progress at this many milliseconds */
    float painted;  /* the progress last painted, -1 before the first paint */
} S = {.freeze = -1, .painted = -1.0f};
static kui_canvas_t S_KC NEMA_EXT_BSS; /* the logical canvas, in external RAM where the board has it */

/* The reference progress: elapsed / 2 s, clamped. */
static float progress_of(uint32_t el) { return el >= SPLASH_MS ? 1.0f : (float)el / (float)SPLASH_MS; }

static void paint(pal_canvas_t *c, float p) {
    kui_canvas_init(&S_KC, c->w, c->h, kui_scale_q()); /* all paper */
    kui_splash_draw(&S_KC, p);
    kui_present(c, &S_KC, (pal_rect_t){0, 0, c->w, c->h});
    S.painted = p;
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)a, (void)full;
    paint(c, S.freeze >= 0 ? progress_of((uint32_t)S.freeze) : S.painted < 0 ? 0.0f : S.painted);
}

static void enter(pal_view_t *v) {
    (void)v;
    S.started = false;
    S.painted = -1.0f;
}

/* The splash is done: the boot one leaves (it is an overlay, so the desktop under it shows), the restart one restarts the board. */
static void finish(void) {
    if (S.restart) {
        pal_reboot();
        return;
    }
    pal_desktop_overlay_remove(&pal_splash_view);
}

/* Keys do nothing while the splash is up: it takes them all, so none reach the desktop's bindings. */
static bool splash_key(pal_view_t *v, const pal_key_t *k) {
    (void)v, (void)k;
    return true;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v;
    if (!S.started) {
        S.started = true;
        S.t0 = now;
    }
    uint32_t el = now - S.t0;
    if (S.freeze >= 0) {
        if (S.painted != progress_of((uint32_t)S.freeze)) pal_desktop_invalidate_all();
        return;
    }
    if (S.restart) {
        if (el >= SPLASH_MS) {
            finish();
            return;
        }
    } else {
        size_t done = pal_boot_stage_count();
        if ((el >= SPLASH_MS && done >= SPLASH_TOTAL) || el >= SPLASH_MAX_MS) {
            finish();
            return;
        }
    }
    float p = progress_of(el);
    if (p == S.painted) return;
    paint(pal_display_canvas(), p);
}

/* uishow splash MS: the bar at MS milliseconds, held (the golden images). */
void pal_ui_splash_freeze(int ms) {
    S.freeze = ms;
    pal_desktop_invalidate_all();
}

/* An overlay, not a front view: the app switcher lists the front views, and the splash must not be one of them. */
void pal_ui_splash_show(void) {
    if (pal_desktop_overlay_top() == &pal_splash_view) return;
    S.restart = false;
    pal_desktop_overlay_push(&pal_splash_view);
}

void pal_ui_splash_restart(void) {
    if (!pal_desktop_view("splash")) { /* no desktop (no display): restart now */
        pal_reboot();
        return;
    }
    S.restart = true;
    S.started = false;
    S.freeze = -1;
    if (pal_desktop_overlay_top() != &pal_splash_view) pal_desktop_overlay_push(&pal_splash_view);
}

pal_view_t pal_splash_view = {
    .id = "splash", .title = "Starting", .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_HIDDEN | PAL_VIEW_OPAQUE, .enter = enter,
    .draw = draw, .key = splash_key, .tick = tick, .tick_ms = SPLASH_TICK_MS,
};
