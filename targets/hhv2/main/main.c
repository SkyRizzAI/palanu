/* PalanuOS for the hhv2 board. The board's hardware is brought up by pal_boot (hw_init); the services are registered
 * by the platform (platform_services) and started by the supervisor in `after` order. This file only describes the board. */
#include <string.h>

#include "hhv2.h"
#include "pal/desktops.h"
#include "pal/svc.h"
#include "pal_services_esp.h"

/* The keyboard scan task: the input service starts it once the input layer is up (boot). */
static nema_err_t input_start(void) {
    hhv2_keyboard_start();
    return NEMA_OK;
}

static void platform_services(void) {
    pal_esp_register_services();
    psys_svc_builtin_t in;
    memset(&in, 0, sizeof(in));
    in.id = "input";
    in.name = "Keyboard input";
    in.restart = PSYS_SVC_RESTART_NEVER;
    in.autostart = true;
    in.core = true;
    in.start = input_start;
    psys_svc_register_builtin(&in);
}

/* The desktop sessions picked at build time (PALANU_DESKTOPS in main/CMakeLists.txt). */
static const pal_desktop_env_t *const DESKTOPS[] = {PAL_DESKTOP_LIST NULL};

void app_main(void) {
    pal_boot_cfg_t cfg = {
        .board = &HHV2_BOARD,
        .storage_root = hhv2_storage_init(),
        .hw_init = hhv2_hw_init,
        .display = hhv2_display_ops(),
        .console_log = true,
        .bootcount = pal_bootcount_esp(),
        .platform_services = platform_services,
        .sd = hhv2_sd_ops(),
        .desktops = DESKTOPS,
    };
    pal_boot(&cfg);
}
