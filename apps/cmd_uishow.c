/* uishow: opens a screen in a fixed state, for the golden images and for looking at the screens (dev command). The clock is
 * set to 2026-01-01 10:09 and, for the notification center, four sample notifications are posted with times that give the
 * labels 09:04, 11m, 4m and 1m. The screen opens through the desktop's pump (pal_ui_request_view), so it is safe from this
 * task. The screens are in system/src/ui_*.c; docs/UI.md lists them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/anim.h"
#include "pal/desktop.h"
#include "pal/dpm.h"
#include "pal/input.h"
#include "pal/input_menu.h"
#include "pal/notify.h"
#include "pal/settings.h"
#include "pal/time.h"
#include "pal/ui.h"

/* Sets the clock so that local time reads y-m-d h:mi (the time zone is applied the way pal_time_local applies it). */
static void set_local(int y, int mo, int d, int h, int mi) {
    pal_tm_t tm = {0};
    tm.year = y, tm.month = mo, tm.day = d, tm.hour = h, tm.minute = mi;
    pal_time_set(pal_time_to_epoch(&tm) - (uint32_t)(pal_time_tz_minutes() * 60));
}

static void samples(void) {
    psys_notify_clear();
    set_local(2026, 1, 1, 9, 4);
    psys_notify_post("monitor", "Memory", "Internal RAM is below 40 KB free.", 0);
    set_local(2026, 1, 1, 9, 58);
    psys_notify_post("settings", "Time set", "Clock set to 2026-01-01 10:00 by the user.", 0);
    set_local(2026, 1, 1, 10, 5);
    psys_notify_post("terminal", "Script finished", "backup.sh completed in 4.2 s with no errors. See the log for details.", 0);
    set_local(2026, 1, 1, 10, 8);
    psys_notify_post("system", "Wi-Fi connected", "Joined Home-5G with address 192.168.1.20.", 0);
}

static int cmd_uishow(int argc, char **argv) {
    static const char usage[] =
        "usage: uishow home [anim:NAME] [FRAME] [fit=center|fit|stretch|crop] [anchor=NAME] [bar=0|1] [sleep=MS] |\n"
        "               launcher [strip|grid|compact|list] [ref] | applist [ref] | lock [FRAME] | control |\n"
        "               notifications [empty] | gallery | settings [PAGE] | splash [MS] | animsheet NAME [PAGE] |\n"
        "               screen NAME (eject-sd | logs-clear | stop-server | reboot-bootloader | files-delete | toast | popup |\n"
        "               permission | sign-consent) | inputmenu [caps] [sym] [num] [ctrl]\n";
    if (argc < 2) {
        pal_eprintf("%s", usage);
        return 1;
    }
    const char *what = argv[1];
    set_local(2026, 1, 1, 10, strcmp(what, "home") ? 9 : 0); /* the reference desktop screens are at 10:00 */
    if (!strcmp(what, "home")) {
        /* home [anim:NAME] [FRAME] [fit=NAME] [anchor=NAME] [bar=0|1]: the reference desktop with a wallpaper, fit and anchor (the
         * Desktop Setting), and the status bar on or off. Each option is optional and keeps the saved setting when left out. */
        int frame = -1;
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            if (!strncmp(a, "anim:", 5)) {
                if (!pal_ui_set_wallpaper_anim(a + 5)) {
                    pal_eprintf("uishow: no animation %s\n", a + 5);
                    return 1;
                }
            } else if (!strncmp(a, "fit=", 4)) {
                int f = !strcmp(a + 4, "center") ? 0 : !strcmp(a + 4, "fit") ? 1 : !strcmp(a + 4, "stretch") ? 2 : !strcmp(a + 4, "crop") ? 3 : -1;
                if (f < 0 || !pal_ui_set_wall_fit(f)) { pal_eprintf("%s", usage); return 1; }
            } else if (!strncmp(a, "anchor=", 7)) {
                int an = -1;
                for (int k = 0; k < 9; k++)
                    if (!strcmp(a + 7, pal_ui_wall_anchor_name(k))) an = k;
                if (an < 0 || !pal_ui_set_wall_anchor(an)) { pal_eprintf("%s", usage); return 1; }
            } else if (!strncmp(a, "bar=", 4)) {
                psys_settings_kv_load("ui.statusbar", a[4] == '0' ? "0" : "1");
                psys_settings_kv_apply();
            } else if (!strncmp(a, "sleep=", 6)) { /* the display sleep timeout in ms (the reference shot sets 1 s) */
                pal_dpm_set_sleep_ms((uint32_t)atoi(a + 6));
            } else if (a[0] >= '0' && a[0] <= '9') {
                frame = atoi(a); /* the playback position, held (golden images) */
            } else {
                pal_eprintf("%s", usage);
                return 1;
            }
        }
        pal_ui_home_freeze(frame);
        pal_ui_request_view("home");
    } else if (!strcmp(what, "launcher")) {
        /* launcher [strip|grid|compact|list] [ref]: ref shows the reference entries (launcher_fixture.c) */
        int style = PAL_UI_LAUNCHER_STRIP;
        bool fixture = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "grid")) style = PAL_UI_LAUNCHER_GRID;
            else if (!strcmp(argv[i], "compact")) style = PAL_UI_LAUNCHER_COMPACT;
            else if (!strcmp(argv[i], "list")) style = PAL_UI_LAUNCHER_LIST;
            else if (!strcmp(argv[i], "ref")) fixture = true;
        }
        set_local(2026, 1, 1, 10, 0); /* The reference launcher shots are at 10:00 */
        pal_ui_set_launcher(style);
        pal_ui_launcher_fixture(fixture);
        pal_ui_request_view("launcher");
    } else if (!strcmp(what, "applist")) {
        /* applist [ref]: ref shows the reference app list (launcher_fixture.c) */
        set_local(2026, 1, 1, 10, 0); /* The reference app list shot is at 10:00 */
        pal_ui_applist_fixture(argc > 2 && !strcmp(argv[2], "ref"));
        pal_ui_request_view("applist");
    } else if (!strcmp(what, "lock")) {
        pal_ui_lock_freeze(argc > 2 ? atoi(argv[2]) : -1); /* FRAME holds the animation at one playback position */
        pal_ui_request_view("lock");
    } else if (!strcmp(what, "control")) {
        pal_ui_request_view("control");
    } else if (!strcmp(what, "notifications")) {
        if (argc > 2 && !strcmp(argv[2], "empty")) psys_notify_clear();
        else samples();
        set_local(2026, 1, 1, 10, 9);
        pal_ui_request_view("notifications");
    } else if (!strcmp(what, "gallery")) {
        pal_ui_request_view("gallery");
    } else if (!strcmp(what, "settings")) {
        set_local(2026, 1, 1, 10, 0); /* The reference Settings shots are at 10:00 */
        pal_settings_request_page(argc > 2 ? argv[2] : "root");
        pal_ui_request_view("settings");
    } else if (!strcmp(what, "splash")) {
        /* splash [MS] [restart]: the boot splash, or the restart one, with the bar held at MS milliseconds */
        if (argc > 3 && !strcmp(argv[3], "restart")) pal_ui_splash_restart();
        else pal_ui_splash_show();
        pal_ui_splash_freeze(argc > 2 ? atoi(argv[2]) : -1);
    } else if (!strcmp(what, "animsheet") && argc > 2) {
        pal_ui_anim_sheet(argv[2], argc > 3 ? atoi(argv[3]) : 0);
    } else if (!strcmp(what, "inputmenu")) {
        /* inputmenu [caps] [sym] [num] [ctrl]: the Fn double tap menu, with the named keyboard modes turned on first */
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "caps")) pal_input_set_mode(PAL_INPUT_CAPS, true);
            else if (!strcmp(argv[i], "sym")) pal_input_set_mode(PAL_INPUT_SYM, true);
            else if (!strcmp(argv[i], "num")) pal_input_set_mode(PAL_INPUT_NUM, true);
            else if (!strcmp(argv[i], "ctrl")) pal_input_set_mode(PAL_INPUT_CTRL, true);
            else {
                pal_eprintf("uishow: no keyboard mode %s\n", argv[i]);
                return 1;
            }
        }
        if (argc == 2) { /* the plain menu starts from a clean keyboard */
            for (int m = 0; m < PAL_INPUT_MODES; m++) pal_input_set_mode((pal_input_mode_t)m, false);
        }
        pal_input_menu_open();
    } else if (!strcmp(what, "screen") && argc > 2) {
        /* screen NAME: a dialog or page of the reference screens (eject-sd, logs-clear, stop-server, reboot-bootloader,
         * files-delete, toast, popup, permission, sign-consent); the clock reads 10:00 as in the reference shots */
        set_local(2026, 1, 1, 10, 0);
        if (!pal_ui_ref_fixture(argv[2])) {
            pal_eprintf("uishow: no reference screen %s\n", argv[2]);
            return 1;
        }
    } else {
        pal_eprintf("%s", usage);
        return 1;
    }
    pal_printf("uishow: %s\n", what);
    return 0;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t UISHOW_APPS[] = {
    APP("uishow", "open a screen in a fixed state for the golden images (home, launcher, lock, control, notifications, gallery)", cmd_uishow, 8192),
};

void pal_apps_register_uishow(void);
void pal_apps_register_uishow(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(UISHOW_APPS); i++) nema_app_register(&UISHOW_APPS[i]);
}
