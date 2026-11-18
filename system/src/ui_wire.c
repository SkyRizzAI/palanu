/* Wiring of the screens into the desktop (docs/DESKTOP.md, pal/overlay.h): the home view is the desktop's home, the lock view
 * is the desktop's lock view, and the home keys open the launcher (Enter), the control center (Up) and the notifications
 * (Down) as opaque overlays. Lock and unlock go through the desktop's lock state. Every desktop call the screens need is in
 * this file, so the switch-over stays in one place. */
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/input.h"
#include "pal/overlay.h"
#include "pal/ui.h"

/* The classic desktop: the home view with the clock, and the launcher, control center and notifications on its keys. */
static bool classic_start(void) {
    pal_desktop_set_chrome(NULL); /* the themed status bar, the legend bar, and the compositor's global keys */
    pal_desktop_set_home("home"); /* Fn+Q style home and the Esc fallback go here */
    pal_desktop_bind_home_key(PAL_KEY_ENTER, "launcher");
    pal_desktop_bind_home_key(PAL_KEY_UP, "control");
    pal_desktop_bind_home_key(PAL_KEY_DOWN, "notifications");
    return true;
}

static const pal_desktop_env_t CLASSIC = {.id = "classic", .title = "Classic", .about = "clock home, launcher on Enter", .start = classic_start};

/* The console: no desktop at all, as on a system without a graphical session. The Terminal fills the screen with no status bar, and the
 * global keys do nothing (the shell has its own: Ctrl+C, Ctrl+D, history). `desktop use classic` brings a desktop back. */
static bool console_key(const pal_key_t *k) {
    if (k->sym == PAL_KEY_VKB || k->sym == PAL_KEY_SWITCH) return true; /* no menus without a desktop */
    if (k->sym == PAL_KEY_GOHOME) {
        pal_desktop_home();
        return true;
    }
    return false;
}

static const pal_desktop_chrome_t CONSOLE_CHROME = {.hide_bar = true, .hide_legend = true, .key = console_key};

static bool console_start(void) {
    if (!pal_desktop_view("terminal")) return false;
    pal_desktop_set_chrome(&CONSOLE_CHROME);
    pal_desktop_set_home("terminal");
    pal_desktop_bind_home_key(PAL_KEY_ENTER, NULL);
    pal_desktop_bind_home_key(PAL_KEY_UP, NULL);
    pal_desktop_bind_home_key(PAL_KEY_DOWN, NULL);
    return true;
}

static const pal_desktop_env_t CONSOLE = {.id = "console", .title = "Console", .about = "no desktop: the shell on the whole screen", .start = console_start};

void pal_ui_register_all(void) {
    pal_ui_prefs_load();
    pal_desktop_register(&pal_home_view);
    pal_desktop_register(&pal_lock_view);
    pal_desktop_register(&pal_control_view);
    pal_desktop_register(&pal_notify_view);
    pal_desktop_register(&pal_gallery_view);
    pal_desktop_register(&pal_splash_view);    /* the boot splash is the front view at start (boot.c) */
    pal_desktop_register(&pal_animplay_view);  /* anim play */
    pal_desktop_register(&pal_animsheet_view); /* anim sheet */
    pal_desktop_set_lock_view("lock");     /* pal_desktop_lock() pushes it; it gets every key until unlock */
    pal_desktops_register(&CLASSIC);
    pal_desktops_register(&CONSOLE);
}

void pal_ui_lock_now(void) { (void)pal_desktop_lock(); }

void pal_ui_unlock(void) { pal_desktop_unlock(); }
