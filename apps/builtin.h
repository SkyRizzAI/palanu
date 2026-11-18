/* The built-in image: every app and view that ships in firmware, and the boot sequence that composes them. */
#ifndef PAL_BUILTIN_H
#define PAL_BUILTIN_H

#include "pal/bootcount.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/input.h"
#include "pal/sd.h"

typedef struct {
    const pal_board_desc_t *board; /* the board: capabilities, geometry, keyboard and hooks. Must not be NULL */
    pal_display_ops_t display;     /* the panel driver; width and height come from board->display_w/h */
    const char *storage_root; /* directory of the persistent filesystem (e.g. "/lfs"), or NULL for RAM only */
    void (*hw_init)(void); /* board hardware that needs the kernel (memory owners, tasks); runs after nema_init */
    bool console_log;      /* echo the kernel log to the debug console */
    pal_status_t (*status)(void);
    const pal_bootcount_ops_t *bootcount; /* the platform's boot-attempt counter; NULL: none, never safe mode */
    void (*platform_services)(void);      /* registers the board's built-in services (input, link-uart, time, ota-confirm); before the start */
    const pal_sd_ops_t *sd;               /* the memory card slot (pal/sd.h); NULL: none. Mounted at /sd during boot */
} pal_boot_cfg_t;

/* Bring the whole system up: kernel, storage, display, input, desktop. Returns once the desktop runs. */
void pal_boot(const pal_boot_cfg_t *cfg);

/* Settings persistence (/data/settings). Save after any change; load once at boot. */
void pal_settings_save(void);
void pal_settings_load(void);
void pal_settings_load_radio(void); /* radio.wifi and radio.ble only; boot reads these even without a display */
const char *pal_settings_radio_wifi(void); /* auto | off | on */
const char *pal_settings_radio_ble(void); /* off | on */
void pal_settings_set_radio(const char *key, const char *value); /* "radio.wifi" or "radio.ble"; saves */
void pal_settings_load_boot(void);          /* boot.target; read at boot before the desktop starts */
const char *pal_settings_boot_target(void); /* desktop | console */
bool pal_settings_set_boot_target(const char *value); /* saves; false for a value that is neither */
void pal_settings_request_page(const char *name); /* Settings opens on this page (uishow settings NAME); "root" is the root */
void pal_settings_bind_host(void); /* hands the app-owned settings to the system settings table (settings_store.c); boot */

/* Views (graphical built-in apps). */
extern pal_view_t pal_terminal_view;
extern pal_view_t pal_settings_view;
extern pal_view_t pal_monitor_view;

/* Opens the Terminal and types cmdline into its shell (the launcher's installed tty apps; terminal.c). */
void pal_terminal_run(const char *cmdline);

#endif
