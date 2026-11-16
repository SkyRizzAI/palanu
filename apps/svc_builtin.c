/* The built-in services that need only portable APIs: the two radios, Palanu Link over TCP and over BLE, and the
 * desktop. Each is a thin wrapper over the existing start and stop functions, registered with the supervisor before
 * boot starts the services in `after` order. The board's services (input, link-uart, time, ota-confirm) are registered by
 * the platform through pal_boot_cfg_t.platform_services.
 *
 * Radio services keep their enabled choice in the radio settings (docs/RADIO.md): radio-wifi autostarts when radio.wifi is
 * on, or auto with saved credentials; radio-ble when radio.ble is on. Holders: "svc" for the radio services, "link" for
 * link-ble, so a link that needs BLE keeps the radio on while it runs. */
#include <string.h>

#include "builtin.h"
#include "pal/link.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/wifi.h"

#define LINK_TCP_PORT 7000
#define LINK_WS_PORT 7001 /* WebSocket: Web Forge in a browser (forge/web) */
#define HOLD_SVC "svc"
#define HOLD_LINK "link"

static const char *const AFTER_WIFI[] = {"radio-wifi", NULL};
static const char *const AFTER_BLE[] = {"radio-ble", NULL};

static nema_err_t wifi_start(void) { return pal_radio_request(PAL_RADIO_WIFI, HOLD_SVC); }
static void wifi_stop(void) { (void)pal_radio_release(PAL_RADIO_WIFI, HOLD_SVC); }
static void wifi_enabled(bool on) { pal_settings_set_radio("radio.wifi", on ? "on" : "off"); }

static nema_err_t ble_start(void) { return pal_radio_request(PAL_RADIO_BLE, HOLD_SVC); }
static void ble_stop(void) { (void)pal_radio_release(PAL_RADIO_BLE, HOLD_SVC); }
static void ble_enabled(bool on) { pal_settings_set_radio("radio.ble", on ? "on" : "off"); }

static nema_err_t link_ble_start(void) { return pal_radio_request(PAL_RADIO_BLE, HOLD_LINK); }
static void link_ble_stop(void) { (void)pal_radio_release(PAL_RADIO_BLE, HOLD_LINK); }

static nema_err_t link_tcp_start(void) { return pal_link_serve_tcp(LINK_TCP_PORT); }
static void link_tcp_stop(void) { pal_link_stop_tcp(); }
static nema_err_t link_ws_start(void) { return pal_link_serve_ws(LINK_WS_PORT); }
static void link_ws_stop(void) { pal_link_stop_ws(); }

/* The TCP link serves only while the station holds an address (docs/RADIO.md). */
static bool has_address(void) {
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    return st.connected;
}

static bool wifi_wanted_at_boot(void) {
    const char *w = pal_settings_radio_wifi();
    return !strcmp(w, "on") || (!strcmp(w, "auto") && pal_wifi_has_credentials());
}

/* Registers the radio and link services. Reads the radio settings first: they decide the autostart. */
void pal_svc_register_radio_link(void) {
    pal_settings_load_radio();
    psys_svc_builtin_t b;
    memset(&b, 0, sizeof(b));
    b.id = "radio-wifi";
    b.name = "Wi-Fi radio";
    b.restart = PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = wifi_wanted_at_boot();
    b.start = wifi_start;
    b.stop = wifi_stop;
    b.on_enabled = wifi_enabled;
    psys_svc_register_builtin(&b);

    memset(&b, 0, sizeof(b));
    b.id = "radio-ble";
    b.name = "Bluetooth radio";
    b.restart = PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = !strcmp(pal_settings_radio_ble(), "on");
    b.start = ble_start;
    b.stop = ble_stop;
    b.on_enabled = ble_enabled;
    psys_svc_register_builtin(&b);

    memset(&b, 0, sizeof(b));
    b.id = "link-tcp";
    b.name = "Palanu Link over TCP";
    b.after = AFTER_WIFI;
    b.restart = PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = true;
    b.start = link_tcp_start;
    b.stop = link_tcp_stop;
    b.gate = has_address;
    b.waiting_for = "a Wi-Fi address";
    psys_svc_register_builtin(&b);

    memset(&b, 0, sizeof(b));
    b.id = "link-ws";
    b.name = "Palanu Link over WebSocket";
    b.after = AFTER_WIFI;
    b.restart = PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = true;
    b.start = link_ws_start;
    b.stop = link_ws_stop;
    b.gate = has_address;
    b.waiting_for = "a Wi-Fi address";
    psys_svc_register_builtin(&b);

    memset(&b, 0, sizeof(b));
    b.id = "link-ble";
    b.name = "Palanu Link over BLE";
    b.after = AFTER_BLE;
    b.restart = PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = !strcmp(pal_settings_radio_ble(), "on"); /* follows the BLE radio setting */
    b.start = link_ble_start;
    b.stop = link_ble_stop;
    psys_svc_register_builtin(&b);
}

/* The desktop is core: it is running once boot has brought the display up, and it cannot be stopped. */
void pal_svc_register_desktop(void) {
    psys_svc_builtin_t b;
    memset(&b, 0, sizeof(b));
    b.id = "desktop";
    b.name = "Desktop";
    b.restart = PSYS_SVC_RESTART_NEVER;
    b.autostart = true;
    b.core = true;
    psys_svc_register_builtin(&b);
}
