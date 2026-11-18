/* The classic desktop session (ADR-0010): the pixel desktop's own screens and keys. Its start registers its screens (home, lock,
 * launcher, app list, control center, notifications, splash), which makes them its own: its stop (desktops.c) takes all of them
 * away, so no classic screen exists while another session runs. Its keys come first after the system's and the lock's: Fn twice
 * opens the input menu, a tap of the cycle key the switcher, and on the home Enter, Up and Down open the launcher, the control
 * center and the notifications as opaque overlays. Lock and unlock go through the compositor's lock state. */
#include "desktop_int.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/input.h"
#include "pal/input_menu.h"
#include "pal/overlay.h"
#include "pal/ui.h"

static bool classic_key(const pal_key_t *k) {
    if (k->sym == PAL_KEY_VKB) { /* Fn tapped twice: the input menu, and from it the full-page keyboard (pal/input_menu.h) */
        pal_input_menu_fn_double();
        return true;
    }
    if (k->sym == PAL_KEY_SWITCH && ovl_count() == 0) {
        pal_switcher_open();
        return true;
    }
    return false; /* the home bindings, the overlays and the front view come next */
}

static const pal_desktop_chrome_t CLASSIC_CHROME = {.key = classic_key}; /* the themed status bar and legend bar stay */

static bool classic_start(void) {
    pal_view_t *const views[] = {&pal_home_view,   &pal_lock_view,   &pal_launcher_view, &pal_applist_view,
                                 &pal_control_view, &pal_notify_view, &pal_splash_view};
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++)
        if (pal_desktop_register(views[i]) != NEMA_OK) return false;
    pal_desktop_set_chrome(&CLASSIC_CHROME);
    pal_desktop_set_home("home");
    pal_desktop_set_lock_view("lock"); /* pal_desktop_lock() pushes it; it gets every key until unlock */
    pal_desktop_bind_home_key(PAL_KEY_ENTER, "launcher");
    pal_desktop_bind_home_key(PAL_KEY_UP, "control");
    pal_desktop_bind_home_key(PAL_KEY_DOWN, "notifications");
    return true;
}

const pal_desktop_env_t PAL_DESKTOP_CLASSIC = {.id = "classic", .title = "Classic", .about = "themed pixel desktop",
                                             .start = classic_start};

/* The system's own screens (dev tools that every session may open). The sessions are registered by boot from the target's list. */
void pal_ui_register_all(void) {
    pal_ui_prefs_load();
    pal_desktop_register(&pal_gallery_view);   /* uishow gallery */
    pal_desktop_register(&pal_animplay_view);  /* anim play */
    pal_desktop_register(&pal_animsheet_view); /* anim sheet */
}

void pal_ui_lock_now(void) { (void)pal_desktop_lock(); }

void pal_ui_unlock(void) { pal_desktop_unlock(); }
