/* PalanuOS Wi-Fi management. The board provides the operations; everything else (shell commands, status bar,
 * settings) goes through these wrappers, so the simulator can stand in with the host's network. */
#ifndef PAL_WIFI_H
#define PAL_WIFI_H

#include "nema/types.h"

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secure;
} pal_ap_t;

typedef struct {
    bool connected;
    bool saved;     /* credentials are stored and the system reconnects by itself */
    char ssid[33];
    char ip[16];
    int8_t rssi;
    bool time_synced;
} pal_wifi_status_t;

typedef struct {
    nema_err_t (*scan)(pal_ap_t *out, size_t max, size_t *found); /* blocking, a few seconds */
    nema_err_t (*connect)(const char *ssid, const char *pass, int32_t timeout_ms); /* saves on success */
    nema_err_t (*disconnect)(void);
    nema_err_t (*forget)(void); /* disconnect and erase the stored credentials */
    void (*status)(pal_wifi_status_t *st);
    bool (*has_credentials)(void); /* a saved network exists; reads flash only, the radio stays off */
} pal_wifi_ops_t;

void pal_wifi_set_ops(const pal_wifi_ops_t *ops);
bool pal_wifi_available(void);
bool pal_wifi_has_credentials(void);
nema_err_t pal_wifi_scan(pal_ap_t *out, size_t max, size_t *found);
nema_err_t pal_wifi_connect(const char *ssid, const char *pass, int32_t timeout_ms);
nema_err_t pal_wifi_disconnect(void);
nema_err_t pal_wifi_forget(void);
void pal_wifi_status(pal_wifi_status_t *st);
int pal_wifi_bars(void); /* 0 off, 1..4 */

/* One listener for the station's address: called with true when it gets an IP and false when it loses it. Set it
 * before the radio starts. The port reports the changes with pal_wifi_notify_ip(). */
typedef void (*pal_wifi_ip_fn)(bool has_ip);
void pal_wifi_set_ip_listener(pal_wifi_ip_fn fn);
void pal_wifi_notify_ip(bool has_ip);

#endif
