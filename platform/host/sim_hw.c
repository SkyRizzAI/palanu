/* See sim_hw.h. */
#include "sim_hw.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/log.h"
#include "nema/port.h"
#include "pal/device.h"
#include "pal/hid.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- battery ---- */
static int s_battery = -1;
int sim_hw_battery(void) { return s_battery; }
void sim_hw_set_battery(int percent) { s_battery = percent > 100 ? 100 : percent; }
static int dev_battery(void *ctx) {
    (void)ctx;
    return s_battery;
}

/* ---- LEDs, sensors, pins, I2C ---- */
static pal_device_t s_all[1 + SIM_HW_MAX_LEDS + 2 * SIM_HW_MAX_EXTRA + 1 + 2];
static char s_led_id[SIM_HW_MAX_LEDS][8], s_led_name[SIM_HW_MAX_LEDS][12];
static char s_sen_id[SIM_HW_MAX_EXTRA][8], s_sen_name[SIM_HW_MAX_EXTRA][16];
static char s_gpio_id[SIM_HW_MAX_EXTRA][8], s_gpio_name[SIM_HW_MAX_EXTRA][12];
static uint8_t s_led_color[SIM_HW_MAX_LEDS][3];
static int32_t s_sen_milli[SIM_HW_MAX_EXTRA];
static bool s_sen_set[SIM_HW_MAX_EXTRA];
static int s_gpio_level[SIM_HW_MAX_EXTRA];
static int s_i2c[SIM_HW_MAX_I2C];
static int s_ni2c;
static bool s_log;

static int sensor_read(void *ctx, int32_t *milli) {
    int i = (int)(intptr_t)ctx;
    *milli = s_sen_set[i] ? s_sen_milli[i] : 21500 + 1000 * i;
    return 0;
}
static int gpio_read(void *ctx) { return s_gpio_level[(intptr_t)ctx]; }
static int gpio_write(void *ctx, int level) {
    s_gpio_level[(intptr_t)ctx] = level;
    if (s_log) fprintf(stderr, "sim: gpio%d = %d\n", (int)(intptr_t)ctx, level);
    return 0;
}
static bool i2c_has(int addr) {
    for (int i = 0; i < s_ni2c; i++)
        if (s_i2c[i] == addr) return true;
    return false;
}
static int i2c_probe(void *ctx, int addr) {
    (void)ctx;
    return i2c_has(addr);
}
static int bus_xfer(void *ctx, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn) {
    (void)ctx;
    if (!i2c_has(addr)) return -1;
    int reg = txn > 0 ? tx[0] : 0;
    for (int i = 0; i < rxn; i++) rx[i] = (uint8_t)(reg + i); /* a register file where register n holds n */
    return 0;
}
static void led_set(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b) {
    int i = (int)(intptr_t)ctx;
    (void)pixel;
    s_led_color[i][0] = r, s_led_color[i][1] = g, s_led_color[i][2] = b;
    if (s_log) fprintf(stderr, "sim: led%d = %u,%u,%u\n", i, r, g, b);
}

void sim_hw_led_get(int i, uint8_t rgb[3]) {
    if (i < 0 || i >= SIM_HW_MAX_LEDS) return;
    memcpy(rgb, s_led_color[i], 3);
}
void sim_hw_sensor_set(int i, int32_t milli) {
    if (i < 0 || i >= SIM_HW_MAX_EXTRA) return;
    s_sen_milli[i] = milli, s_sen_set[i] = true;
}
int sim_hw_gpio_get(int i) { return i >= 0 && i < SIM_HW_MAX_EXTRA ? s_gpio_level[i] : 0; }
void sim_hw_gpio_set(int i, int level) {
    if (i >= 0 && i < SIM_HW_MAX_EXTRA) s_gpio_level[i] = level ? 1 : 0;
}
void sim_hw_i2c_set(int addr, bool present) {
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_ni2c; i++)
        if (s_i2c[i] == addr) {
            if (!present) s_i2c[i] = s_i2c[--s_ni2c];
            pthread_mutex_unlock(&s_lock);
            return;
        }
    if (present && s_ni2c < SIM_HW_MAX_I2C) s_i2c[s_ni2c++] = addr;
    pthread_mutex_unlock(&s_lock);
}

/* ---- radios ---- */
static nema_err_t radio_start(pal_radio_t r) {
    (void)r;
    return NEMA_OK;
}
static nema_err_t radio_stop(pal_radio_t r) {
    (void)r;
    return NEMA_OK;
}
static const pal_radio_ops_t RADIO = {radio_start, radio_stop, NULL};
const pal_radio_ops_t *sim_hw_radio_ops(void) { return &RADIO; }

void sim_hw_ble_set_name(const char *name) { (void)name; }

/* ---- Wi-Fi ---- */
typedef struct {
    pal_ap_t ap;
    char password[64];
} sim_ap_t;
static sim_ap_t s_aps[SIM_HW_MAX_APS] = {
    {{"MyHomeWiFi", -42, 36, true}, ""},
    {{"CoffeeShop_Free", -68, 6, false}, ""},
    {{"Neighbour_5G", -74, 11, true}, ""},
    {{"AndroidAP", -81, 1, true}, ""},
};
static int s_nap = 4;
static char s_ssid[33];
static char s_ip[16] = "192.168.1.20";
static bool s_refuse, s_connected;
static int s_rssi = -48;

void sim_hw_ap_clear(void) {
    pthread_mutex_lock(&s_lock);
    s_nap = 0;
    pthread_mutex_unlock(&s_lock);
}
int sim_hw_ap_add(const char *ssid, int rssi, int channel, bool secure, const char *password) {
    pthread_mutex_lock(&s_lock);
    int n = s_nap;
    if (n < SIM_HW_MAX_APS) {
        sim_ap_t *a = &s_aps[n];
        memset(a, 0, sizeof(*a));
        snprintf(a->ap.ssid, sizeof(a->ap.ssid), "%s", ssid);
        a->ap.rssi = (int8_t)rssi, a->ap.channel = (uint8_t)channel, a->ap.secure = secure;
        snprintf(a->password, sizeof(a->password), "%s", password ? password : "");
        s_nap = n + 1;
    }
    pthread_mutex_unlock(&s_lock);
    return n < SIM_HW_MAX_APS ? n : -1;
}
void sim_hw_wifi_config(const char *ip, bool refuse_connect) {
    pthread_mutex_lock(&s_lock);
    if (ip && ip[0]) snprintf(s_ip, sizeof(s_ip), "%s", ip);
    s_refuse = refuse_connect;
    pthread_mutex_unlock(&s_lock);
}
bool sim_hw_wifi_connected(void) { return s_connected; }
void sim_hw_wifi_set_connected(bool on) { s_connected = on && s_ssid[0]; }
int sim_hw_wifi_rssi(void) { return s_rssi; }

static nema_err_t wifi_scan(pal_ap_t *out, size_t max, size_t *found) {
    pthread_mutex_lock(&s_lock);
    size_t n = (size_t)s_nap < max ? (size_t)s_nap : max;
    for (size_t i = 0; i < n; i++) out[i] = s_aps[i].ap;
    pthread_mutex_unlock(&s_lock);
    *found = n;
    return NEMA_OK;
}
static nema_err_t wifi_connect(const char *ssid, const char *pass, int32_t timeout_ms) {
    (void)timeout_ms;
    nema_err_t rc = NEMA_OK;
    pthread_mutex_lock(&s_lock);
    const sim_ap_t *hit = NULL;
    for (int i = 0; i < s_nap; i++)
        if (strcmp(s_aps[i].ap.ssid, ssid) == 0) hit = &s_aps[i];
    if (s_refuse) rc = NEMA_ERR_IO;
    else if (hit && hit->ap.secure && hit->password[0] && strcmp(hit->password, pass ? pass : "") != 0) rc = NEMA_ERR_IO;
    if (rc == NEMA_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        s_connected = true;
        s_rssi = hit ? hit->ap.rssi : -60;
    }
    pthread_mutex_unlock(&s_lock);
    if (rc == NEMA_OK) pal_wifi_joined(ssid, pass); /* as the board does: saved, first in the list */
    return rc;
}
static nema_err_t wifi_disconnect(void) {
    s_connected = false;
    return NEMA_OK;
}
static nema_err_t wifi_forget(void) {
    s_connected = false;
    s_ssid[0] = 0;
    return NEMA_OK;
}
static void wifi_status(pal_wifi_status_t *st) {
    st->connected = s_connected;
    st->saved = false;
    snprintf(st->ssid, sizeof(st->ssid), "%s", s_ssid);
    snprintf(st->ip, sizeof(st->ip), "%s", s_connected ? s_ip : "");
    pal_wifi_net_t n;
    if (s_connected && pal_wifi_saved_get(s_ssid, &n) && n.manual_ip) pal_ipv4_text(n.ip.ip, st->ip, sizeof(st->ip));
    st->saved = pal_wifi_saved_get(s_ssid, NULL);
    st->rssi = s_connected ? s_rssi : 0;
    st->time_synced = s_connected;
}
static bool wifi_has_credentials(void) { return false; }
/* The saved networks (pal/wifi.h): kept in memory for the simulator's life, like the board's NVS between restarts. */
static uint8_t s_nets[1024];
static size_t s_nets_len;
static nema_err_t wifi_store(const void *blob, size_t len) {
    if (len > sizeof(s_nets)) return NEMA_ERR_RANGE;
    memcpy(s_nets, blob, len);
    s_nets_len = len;
    return NEMA_OK;
}
static size_t wifi_load(void *blob, size_t cap) {
    size_t n = s_nets_len < cap ? s_nets_len : cap;
    memcpy(blob, s_nets, n);
    return n;
}
static const pal_wifi_ops_t WIFI = {wifi_scan, wifi_connect, wifi_disconnect, wifi_forget, wifi_status, wifi_has_credentials, wifi_store, wifi_load};
const pal_wifi_ops_t *sim_hw_wifi_ops(void) { return &WIFI; }

/* ---- the device table ---- */
/* ---- the keyboard transport (pal/hid.h): "pairs" 0.8 s after it starts, and logs each report (`log | grep hid`) ---- */
static bool s_hid_on;
static uint64_t s_hid_t0;
static uint32_t s_hid_reports;
static uint8_t s_hid_last[7];

static nema_err_t hid_start(const char *name) {
    s_hid_on = true;
    s_hid_t0 = nema_port_time_us();
    NEMA_LOGI("hid", "advertising as \"%s\" (simulated: connects in 0.8 s)", name);
    return NEMA_OK;
}
static void hid_stop(void) {
    if (s_hid_on) NEMA_LOGI("hid", "stopped after %u reports", (unsigned)s_hid_reports);
    s_hid_on = false;
}
static pal_hid_state_t hid_state(void) {
    if (!s_hid_on) return PAL_HID_OFF;
    return nema_port_time_us() - s_hid_t0 < 800000u ? PAL_HID_WAITING : PAL_HID_CONNECTED;
}
static nema_err_t hid_report(uint8_t mods, const uint8_t keys[6]) {
    if (hid_state() != PAL_HID_CONNECTED) return NEMA_ERR_STATE;
    s_hid_reports++;
    s_hid_last[0] = mods;
    memcpy(s_hid_last + 1, keys, 6);
    if (keys[0]) NEMA_LOGI("hid", "report mods %02x key %02x", mods, keys[0]);
    return NEMA_OK;
}
static void hid_peer(char *out, size_t cap) { snprintf(out, cap, "simulated host"); }
static const pal_hid_ops_t SIM_HID = {hid_start, hid_stop, hid_state, hid_report, hid_peer};

uint32_t sim_hw_hid_reports(uint8_t last[7]) {
    if (last) memcpy(last, s_hid_last, 7);
    return s_hid_reports;
}

void sim_hw_init(pal_board_desc_t *board, const sim_hw_cfg_t *cfg) {
    pal_hid_set(&SIM_HID);
    s_log = cfg->log;
    int nled = cfg->leds > SIM_HW_MAX_LEDS ? SIM_HW_MAX_LEDS : cfg->leds;
    int nsen = cfg->sensors > SIM_HW_MAX_EXTRA ? SIM_HW_MAX_EXTRA : cfg->sensors;
    int ngpio = cfg->gpios > SIM_HW_MAX_EXTRA ? SIM_HW_MAX_EXTRA : cfg->gpios;
    if (cfg->i2c && s_ni2c == 0) sim_hw_i2c_set(0x48, true), sim_hw_i2c_set(0x68, true);
    if (nled <= 0 && nsen <= 0 && ngpio <= 0 && !cfg->i2c && !cfg->radios) return;
    int n = 0;
    s_all[n++] = (pal_device_t){.id = "battery0", .name = "Battery", .kind = PAL_DEV_BATTERY, .battery = {.percent = dev_battery}};
    for (int k = 0; k < nled; k++) {
        snprintf(s_led_id[k], sizeof(s_led_id[k]), "led%d", k);
        snprintf(s_led_name[k], sizeof(s_led_name[k]), "LED %d", k + 1);
        s_all[n++] = (pal_device_t){.id = s_led_id[k], .name = s_led_name[k], .kind = PAL_DEV_LED, .ctx = (void *)(intptr_t)k,
                                    .led = {.pixels = 1, .colors = 3, .set = led_set}};
    }
    for (int k = 0; k < nsen; k++) {
        snprintf(s_sen_id[k], sizeof(s_sen_id[k]), "temp%d", k);
        snprintf(s_sen_name[k], sizeof(s_sen_name[k]), "Temperature %d", k + 1);
        s_all[n++] = (pal_device_t){.id = s_sen_id[k], .name = s_sen_name[k], .kind = PAL_DEV_SENSOR, .ctx = (void *)(intptr_t)k,
                                    .sensor = {.unit = "C", .read = sensor_read}};
    }
    for (int k = 0; k < ngpio; k++) {
        snprintf(s_gpio_id[k], sizeof(s_gpio_id[k]), "gpio%d", k);
        snprintf(s_gpio_name[k], sizeof(s_gpio_name[k]), "Pin %d", 10 + k);
        s_all[n++] = (pal_device_t){.id = s_gpio_id[k], .name = s_gpio_name[k], .kind = PAL_DEV_GPIO, .ctx = (void *)(intptr_t)k,
                                    .gpio = {.pin = 10 + k, .app = true, .read = gpio_read, .write = gpio_write}};
    }
    if (cfg->i2c)
        s_all[n++] = (pal_device_t){.id = "i2c0", .name = "I2C bus", .kind = PAL_DEV_BUS,
                                    .bus = {.type = PAL_BUS_I2C, .app = true, .probe = i2c_probe, .xfer = bus_xfer}};
    if (cfg->radios) {
        s_all[n++] = (pal_device_t){.id = "wifi0", .name = "Wi-Fi", .kind = PAL_DEV_RADIO, .radio = {PAL_DEVRADIO_WIFI}};
        s_all[n++] = (pal_device_t){.id = "ble0", .name = "Bluetooth LE", .kind = PAL_DEV_RADIO, .radio = {PAL_DEVRADIO_BLE}};
    }
    board->devices = s_all, board->ndevices = n;
}
