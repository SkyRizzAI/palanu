/* The board's built-in services and the radio glue, registered before the boot starts services:
 *   link-uart  the console and Palanu Link on the debug UART (core: it is the way back in)
 *   time       SNTP, while the station has an address
 *   ota-confirm the health check of a new firmware image (core: it must run in safe mode too)
 * The other services are portable (apps/svc_builtin.c). */
#include <string.h>

#include "pal/ota.h"
#include "pal/ota_confirm.h"
#include "pal/svc.h"
#include "pal/wifi.h"
#include "nema/nema.h"
#include "pal_console.h"
#include "pal_radio_esp.h"
#include "pal_services_esp.h"

static nema_err_t console_start(void) {
    pal_console_start(); /* starts the shell and the link on the UART; blocking-free: its tasks run from here */
    return NEMA_OK;
}

static nema_err_t time_start(void) { return pal_wifi_esp_sntp_start(); }
static void time_stop(void) { pal_wifi_esp_sntp_stop(); }

static bool has_address(void) {
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    return st.connected;
}

static const char *const AFTER_WIFI[] = {"radio-wifi", NULL};
static const char *const AFTER_UI[] = {"desktop", "link-uart", NULL};

static void add(const psys_svc_builtin_t *b) {
    if (psys_svc_register_builtin(b) != NEMA_OK) NEMA_LOGE("svc", "built-in %s not registered", b->id);
}

void pal_power_esp_register(void); /* power_esp.c */

void pal_esp_register_services(void) {
    pal_power_esp_register(); /* power profiles: the CPU frequency policy */
    pal_radio_esp_init();    /* radio ops and the Wi-Fi address hook */
    pal_ota_esp_start();     /* firmware update ops and the health check's ops */
    pal_hid_ble_register();  /* the Keyboard app's transport: a Bluetooth LE keyboard (ADR-0018) */

    psys_svc_builtin_t b;
    memset(&b, 0, sizeof(b));
    b.id = "link-uart";
    b.name = "Palanu Link and console on UART";
    b.restart = PSYS_SVC_RESTART_NEVER;
    b.autostart = true;
    b.core = true;
    b.start = console_start;
    add(&b);

    memset(&b, 0, sizeof(b));
    b.id = "time";
    b.name = "Network time (SNTP)";
    b.after = AFTER_WIFI;
    b.restart = PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = true;
    b.start = time_start;
    b.stop = time_stop;
    b.gate = has_address;
    b.waiting_for = "a Wi-Fi address";
    add(&b);

    memset(&b, 0, sizeof(b));
    b.id = "ota-confirm";
    b.name = "Firmware health check";
    b.after = AFTER_UI;
    b.restart = PSYS_SVC_RESTART_NEVER;
    b.autostart = true;
    b.core = true;
    b.start = psys_ota_confirm_start;
    add(&b);
}
