/* Board glue for the radio manager (pal/radio.h): the table of radio ops, the Wi-Fi status line, and the Wi-Fi address hook.
 * The hook does not start the TCP link itself: it wakes the service supervisor, whose link-tcp and time services are gated
 * on the address (apps/svc_builtin.c, platform/esp-idf/palanu/svc_esp.c). */
#include <stdio.h>

#include "nema/nema.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/wifi.h"
#include "pal_radio_esp.h"

static nema_err_t radio_start(pal_radio_t r) {
    return r == PAL_RADIO_WIFI ? pal_wifi_esp_start() : pal_link_ble_start("PalanuOS");
}

static nema_err_t radio_stop(pal_radio_t r) {
    if (r == PAL_RADIO_WIFI) return pal_wifi_esp_stop();
    pal_link_ble_stop();
    return NEMA_OK;
}

static size_t wifi_detail(char *out, size_t cap) {
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    int n = st.connected ? snprintf(out, cap, "connected to \"%s\", ip %s", st.ssid, st.ip)
                         : snprintf(out, cap, "not connected%s", st.saved ? " (saved network, reconnecting)" : "");
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

static size_t radio_detail(pal_radio_t r, char *out, size_t cap) {
    return r == PAL_RADIO_WIFI ? wifi_detail(out, cap) : pal_link_ble_status(out, cap);
}

/* The station gained or lost its address: the gated services re-check (psys_svc_poke never blocks). */
static void on_wifi_ip(bool has_ip) {
    (void)has_ip;
    psys_svc_poke();
}

static const pal_radio_ops_t RADIO_OPS = {radio_start, radio_stop, radio_detail};

void pal_radio_esp_init(void) {
    pal_wifi_esp_register();
    pal_wifi_set_ip_listener(on_wifi_ip);
    pal_radio_set_ops(&RADIO_OPS);
}
