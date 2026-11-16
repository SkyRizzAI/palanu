#include <string.h>

#include "pal/wifi.h"

static pal_wifi_ops_t W;
static bool have;
static pal_wifi_ip_fn s_ip_fn;

void pal_wifi_set_ops(const pal_wifi_ops_t *ops) {
    W = *ops;
    have = true;
}
bool pal_wifi_available(void) { return have; }
bool pal_wifi_has_credentials(void) { return have && W.has_credentials && W.has_credentials(); }
void pal_wifi_set_ip_listener(pal_wifi_ip_fn fn) { s_ip_fn = fn; }
void pal_wifi_notify_ip(bool has_ip) {
    if (s_ip_fn) s_ip_fn(has_ip);
}

nema_err_t pal_wifi_scan(pal_ap_t *out, size_t max, size_t *found) {
    return have && W.scan ? W.scan(out, max, found) : NEMA_ERR_NOSYS;
}
nema_err_t pal_wifi_connect(const char *ssid, const char *pass, int32_t timeout_ms) {
    return have && W.connect ? W.connect(ssid, pass, timeout_ms) : NEMA_ERR_NOSYS;
}
nema_err_t pal_wifi_disconnect(void) { return have && W.disconnect ? W.disconnect() : NEMA_ERR_NOSYS; }
nema_err_t pal_wifi_forget(void) { return have && W.forget ? W.forget() : NEMA_ERR_NOSYS; }

void pal_wifi_status(pal_wifi_status_t *st) {
    memset(st, 0, sizeof(*st));
    if (have && W.status) W.status(st);
}

int pal_wifi_bars(void) {
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    if (!st.connected) return 0;
    return st.rssi > -55 ? 4 : st.rssi > -65 ? 3 : st.rssi > -75 ? 2 : 1;
}
