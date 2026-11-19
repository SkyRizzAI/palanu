#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "pal/console.h"
#include "pal/desktops.h"
#include "pal/sd.h"
#include "pal/settings.h"
#include "pal/bootcount.h"
#include "pal/link.h"
#include "pal/papp.h"
#include "pal/power.h"
#include "pal/perf.h"
#include "pal/radio.h"
#include "pal/registry.h"
#include "pal/svc.h"
#include "pal/vk.h"
#include "libapp.h"
#include "views.h" /* files, textview, logs: views and their commands */
#include "pal/theme.h"
#include "pal/ui.h"
#include "pal/time.h"
#include "pal/wifi.h"

void pal_apps_register_files(void);
void pal_apps_register_sys(void);
void pal_apps_register_dev(void); /* dev: the board's devices from the terminal (cmd_dev.c) */
void pal_app_register_sh(void);
void pal_apps_register_net(void);
void pal_apps_register_radio(void);
void pal_apps_register_sd(void);
void pal_apps_register_link(void);
void pal_apps_register_ssh(void);
void pal_apps_register_perf(void);
void pal_apps_register_crypto(void);
void pal_apps_register_app(void);
void pal_apps_register_notify(void);
void pal_apps_register_svc(void);
void pal_apps_register_uishow(void);
void pal_apps_register_anim(void); /* anim: the animation library (cmd_anim.c) */
void pal_apps_register_views(void); /* files, logs, viewfix commands (cmd_views.c) */
void pal_apps_register_vk(void); /* vkeyboard command (cmd_vk.c) */
void pal_apps_register_uidemo(void);
void pal_svc_register_radio_link(void);
void pal_svc_register_desktop(void);

void pal_apps_register_all(void) {
    pal_app_register_sh();
    pal_apps_register_app();
    pal_apps_register_files();
    pal_apps_register_sys();
    pal_apps_register_dev();
    pal_apps_register_net();
    pal_apps_register_radio();
    pal_apps_register_sd();
    pal_apps_register_link(); /* the link command: pairing, clients, cable trust */
    pal_apps_register_ssh();
    pal_apps_register_perf();
    pal_apps_register_crypto();
    pal_apps_register_notify();
    pal_apps_register_svc();
    pal_apps_register_vk();
    pal_apps_register_uidemo(); /* hidden: the test command that opens the dialogs */
    pal_apps_register_uishow(); /* dev: opens a screen in a fixed state (golden images) */
    pal_apps_register_anim();   /* dev: anim list | info | play | sheet | bench */
    pal_apps_register_views();  /* files, logs (terminal commands) and the viewfix fixture (golden images) */
}

static void put_file(const char *path, const char *text) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) == NEMA_OK) {
        nema_obj_write(f, text, strlen(text), 0);
        nema_obj_unref(f);
    }
}

static const char README[] =
    "Welcome to PalanuOS.\n\n"
    "Keys\n"
    "  Fn + W A S D   arrow keys        Fn + Q / E   Home / End\n"
    "  Fn + R / F     Page up / down    Fn + del     Delete\n"
    "  Nm             symbols printed on the keys (tap to latch one key)\n"
    "  Nm + Shift     extras: [ ] { } < > = | \\ ~ & ^ ` on Q W E R T Y U I O P A S D\n"
    "  cycle key      tap: switch app, hold: home\n"
    "  Alt + N        new terminal tab       Alt + W   close tab\n"
    "  Alt + Tab      next tab               Shift + Fn + W/S   scroll history\n\n"
    "Try: help, ls /bin, free, top, theme dark, cat README.txt\n";

static pal_status_t default_status(void) {
    /* BLE shows while the radio policy is on; no card driver reports the slot yet, so sd stays off. */
    pal_status_t st = {0, 0, pal_battery(), pal_wifi_bars(), pal_link_connected(), !strcmp(pal_settings_radio_ble(), "on"), false};
    return st;
}

void pal_boot(const pal_boot_cfg_t *cfg) {
    pal_boot_mark("start");
    nema_config_t nc = {.console_log = cfg->console_log};
    nema_init(&nc);
    pal_time_init();
    pal_boot_mark("kernel");
    nema_net_init();
    NEMA_CHECK(cfg->board != NULL);
    pal_board_set(cfg->board);
    if (cfg->hw_init) cfg->hw_init();
    /* The attempt counter goes up first: a boot that never reaches ready leaves it high, and the third such boot in a row
     * runs in safe mode. */
    bool safe = psys_bootcount_begin(cfg->bootcount);

    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(3u * 1024u * 1024u));
    nema_vfs_mount("/bin", nema_binfs_ops(), NULL);
    static const char *const dirs[] = {"/home", "/etc", "/tmp", "/data", "/apps"};
    static const char *const persistent[] = {"home", "etc", "data", "apps"};
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(dirs); i++) nema_fs_mkdir(dirs[i]);
    if (cfg->storage_root) { /* /home, /etc and /data live on the persistent filesystem and survive a restart */
        for (size_t i = 0; i < NEMA_ARRAY_SIZE(persistent); i++) {
            char host_dir[200], mp[16];
            snprintf(host_dir, sizeof(host_dir), "%s/%s", cfg->storage_root, persistent[i]);
            snprintf(mp, sizeof(mp), "/%s", persistent[i]);
            (void)nema_posixfs_mkdir(host_dir);
            void *pfs = nema_posixfs_create(host_dir);
            if (!pfs) {
                NEMA_LOGE("boot", "storage path too long: %s", host_dir);
                continue;
            }
            nema_vfs_unmount(mp);
            nema_vfs_mount(mp, nema_posixfs_ops(), pfs);
        }
    }
    if (cfg->sd) { /* the card at /sd when one is in the slot; without one this costs the card's start-up timeout */
        pal_sd_set_ops(cfg->sd);
        (void)pal_sd_mount();
    }
    pal_boot_mark("storage");
    nema_stat_t st;
    if (nema_fs_stat("/home/README.txt", &st) != NEMA_OK) put_file("/home/README.txt", README);
    if (nema_fs_stat("/etc/profile", &st) != NEMA_OK)
        put_file("/etc/profile", "# runs at the start of every interactive shell\nalias ll='ls -l'\nalias la='ls -a'\n");

    pal_settings_bind_host(); /* the palanu settings API reaches the terminal font, radio policy and device name */
    pal_settings_load_boot(); /* boot.target: decides the view in front */
    pal_apps_register_all();
    psys_reg_init(PAL_BUILTIN_APPS, PAL_BUILTIN_APPS_COUNT, &PAL_BUILTIN_COMMAND_DEFAULTS); /* views and command defaults */
    if (!safe) pal_papp_scan(); /* installed packages become commands and registry entries; not in safe mode */

    if (pal_has(PAL_CAP_KEYBOARD) && cfg->board->keyboard) {
        pal_input_init(cfg->board->keyboard);
        pal_input_set_clock(pal_input_real_clock);
    }
    pal_display_ops_t panel = cfg->display;
    panel.width = cfg->board->display_w;
    panel.height = cfg->board->display_h;
    bool desktop_up = false;
    if (pal_has(PAL_CAP_DISPLAY) && panel.width > 0 && pal_display_init(&panel) == NEMA_OK) {
        desktop_up = true;
        pal_theme_select(0, false, 1);
        pal_display_set_brightness(80);
        pal_settings_load();
        pal_display_refresh_theme();
        /* The system console is in front from here: the boot log (under a splash when boot.quiet is on), then a desktop session
         * or, with boot.target console, in safe mode or when the session fails, a login shell (ADR-0010, pal/console.h). */
        pal_console_init(!safe && psys_settings_int("boot.quiet", 1) != 0);
        pal_boot_mark("display");
        pal_console_printf("[\x1b[32m  OK  \x1b[0m] Display %dx%d.\n", panel.width, panel.height);
        pal_desktop_cfg_t dc = {.status = cfg->status ? cfg->status : default_status, .start_view = NULL /* the session's home */};
        pal_desktop_init(&dc);
        pal_ui_register_all(); /* the system's dev screens (ui_wire.c); a session registers its screens at its start */
        for (const pal_desktop_env_t *const *d = cfg->desktops; d && *d; d++) pal_desktops_register(*d); /* what this firmware ships */
        pal_desktop_register(&pal_terminal_view);
        pal_desktop_register(&pal_settings_view);
        pal_desktop_register(&pal_monitor_view);
        pal_desktop_register(&pal_files_view);    /* Files: launcher app */
        pal_desktop_register(&pal_textview_view); /* the text viewer, an overlay opened by Files */
        pal_desktop_register(&pal_logs_view);     /* Logs: launcher app */
        pal_desktop_register(&pal_editor_view);   /* Editor: launcher app, `edit FILE`, Files > Edit */
        pal_vk_register(); /* the on-screen keyboard overlay (vk.h) */
        pal_launcher_set_runner(pal_terminal_run); /* installed tty apps open in the Terminal */
        pal_desktops_boot(); /* the session named by desktop.env, unless boot.target is console or this is safe mode */
        pal_desktop_start();
        pal_boot_mark("desktop");
    }
    /* Background work is services now: registered here, started in `after` order. The radios start after the
     * desktop, so admission sees the memory the system really holds. */
    pal_svc_register_radio_link();
    if (desktop_up) pal_svc_register_desktop();
    if (cfg->platform_services) cfg->platform_services();
    (void)pal_power_apply(psys_settings_int("power.profile", PAL_POWER_BALANCED)); /* the stored profile, now the board's ops exist */
    if (safe) {
        NEMA_LOGE("boot", "*** SAFE MODE ***  %lu boot attempts without reaching ready", (unsigned long)psys_bootcount_value());
        NEMA_LOGE("boot", "only core services run; installed apps are not scanned; `safemode off` clears it");
    }
    psys_svc_start_all(safe);
    pal_boot_mark("services");
    NEMA_LOGI("boot", "PalanuOS up on %s%s", cfg->board->name ? cfg->board->name : "?", safe ? " (safe mode)" : "");
}
