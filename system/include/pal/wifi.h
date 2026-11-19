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
    /* Appended later: the saved networks as one opaque blob the system writes (pal_wifi_saved...). The board keeps it where the
     * shell and Palanu Link cannot read it (NVS on the board), since it holds the passwords. load returns its length, 0 for none. */
    nema_err_t (*store)(const void *blob, size_t len);
    size_t (*load)(void *blob, size_t cap);
} pal_wifi_ops_t;

/* ---- saved networks (appended later) ----
 * Up to PAL_WIFI_NETS networks, the most recently joined first. A network that joins is saved (or moved to the front); the board
 * joins the first one with Auto-Join on at start, and goes on to the next after a few failed tries. Each network may have a
 * manual IPv4 address instead of DHCP. Passwords never leave the system: what these calls return has none. */
#define PAL_WIFI_NETS 8

typedef struct {
    uint8_t ip[4], mask[4], gw[4], dns[4]; /* a, b, c, d */
} pal_ipv4_conf_t;

typedef struct {
    char ssid[33];
    bool auto_join;   /* the board joins it by itself */
    bool manual_ip;   /* use ip below, not DHCP */
    bool secure;      /* it has a password */
    pal_ipv4_conf_t ip;
} pal_wifi_net_t;

size_t pal_wifi_saved(pal_wifi_net_t *out, size_t max);   /* the saved networks, most recent first */
bool pal_wifi_saved_get(const char *ssid, pal_wifi_net_t *out);
nema_err_t pal_wifi_forget_net(const char *ssid);         /* NEMA_ERR_NOENT when it is not saved; disconnects when it is the one in use */
nema_err_t pal_wifi_set_auto(const char *ssid, bool on);
nema_err_t pal_wifi_set_ip(const char *ssid, bool manual, const pal_ipv4_conf_t *conf); /* conf may be NULL when manual is false */
/* Dotted text and back (a, b, c, d); parse refuses anything that is not four numbers 0 to 255. */
bool pal_ipv4_parse(const char *text, uint8_t out[4]);
void pal_ipv4_text(const uint8_t ip[4], char *out, size_t cap);

/* For the board port: the index-th network with Auto-Join on (most recent first), with its password; false past the last one.
 * pal_wifi_joined saves a network that has just joined (or moves it to the front). */
bool pal_wifi_pick(int index, char ssid[33], char pass[65]);
void pal_wifi_joined(const char *ssid, const char *pass);

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
