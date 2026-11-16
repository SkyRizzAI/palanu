/* Wi-Fi station for the board: esp_wifi + esp_netif, credentials in NVS, SNTP once an address arrives.
 * The radio manager starts and stops it. A start brings the driver up fully and a stop takes it fully down, so its
 * RAM goes back to the heap (docs/RADIO.md). What cannot be undone (the LwIP stack, the default event loop, NVS) is
 * set up once per boot. */
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nema/nema.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pal/time.h"
#include "pal/wifi.h"
#include "pal_radio_esp.h"

#define GOT_IP_BIT BIT0
#define FAIL_BIT BIT1
#define DISC_BIT BIT2 /* the station reported a disconnect (w_scan waits for it) */

static EventGroupHandle_t s_ev;
static esp_netif_t *s_netif;      /* created by each start, destroyed by each stop */
static bool s_base_up;            /* NVS, event loop and netif stack: once per boot */
static bool s_started;            /* driver is up */
static bool s_connected, s_saved, s_synced, s_want, s_sntp;
static char s_ip[16], s_ssid[33];
static int s_retry;
static bool s_joining;            /* w_connect is waiting: its failures end in FAIL_BIT, not in a reconnect */
static volatile bool s_scanning;  /* w_scan holds the radio: a disconnect it caused does not reconnect */
static TimerHandle_t s_reconnect; /* the next try at the saved network, with a growing delay */
static int s_backoff_s;           /* that delay: 2, 4, 8, 16, then 30 s */

/* The saved network is out of reach: try again later, not at once. Retrying at once kept the station connecting all the
 * time, and the driver refuses a scan while it connects ("wifi scan failed: i/o error", found on the board). */
static void schedule_reconnect(void) {
    s_backoff_s = s_backoff_s <= 0 ? 2 : s_backoff_s * 2 > 30 ? 30 : s_backoff_s * 2;
    if (!s_reconnect) return;
    xTimerChangePeriod(s_reconnect, pdMS_TO_TICKS(s_backoff_s * 1000), 0); /* also starts it */
}

static void reconnect_due(TimerHandle_t t) {
    (void)t;
    if (s_started && s_want && !s_connected && !s_scanning) esp_wifi_connect();
}

static void sntp_synced(struct timeval *tv) {
    pal_time_set((uint32_t)tv->tv_sec);
    s_synced = true;
    NEMA_LOGI("wifi", "clock set from network time");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = 0;
        pal_wifi_notify_ip(false);
        xEventGroupSetBits(s_ev, DISC_BIT);
        if (s_scanning) {
            /* w_scan stopped a connect attempt to scan; it resumes when the scan is done */
        } else if (s_want && s_retry < 5) { /* the first few failures retry at once; connect() reports the outcome */
            s_retry++;
            esp_wifi_connect();
        } else if (s_want && s_saved && !s_joining) {
            schedule_reconnect();
        } else {
            xEventGroupSetBits(s_ev, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        s_backoff_s = 0;
        xEventGroupSetBits(s_ev, GOT_IP_BIT);
        pal_wifi_notify_ip(true); /* the time service (SNTP) and the TCP link start from the supervisor, see svc_esp.c */
    }
}

/* NVS holds the saved network under the "wifi" namespace. It is read without the driver, so the boot can ask whether
 * Wi-Fi has anything to do before it starts the radio. */
static void nvs_ready(void) {
    static bool done;
    if (done) return;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    done = true;
}

static void nvs_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_load(char *ssid, size_t sc, char *pass, size_t pc) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &sc) == ESP_OK && nvs_get_str(h, "pass", pass, &pc) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool w_has_credentials(void) {
    nvs_ready();
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = nvs_get_str(h, "ssid", NULL, &len) == ESP_OK && len > 1;
    nvs_close(h);
    return ok;
}

/* Goes back to the saved or requested network after a scan stopped the attempt. */
static void resume_after_scan(void) {
    if (s_want && !s_connected) esp_wifi_connect();
}

static nema_err_t w_scan(pal_ap_t *out, size_t max, size_t *found) {
    if (!s_started) return NEMA_ERR_STATE;
    wifi_scan_config_t sc = {.show_hidden = false};
    s_scanning = true;
    if (s_reconnect) xTimerStop(s_reconnect, 0);
    if (s_want && !s_connected) {
        /* The station may be connecting, and then the driver refuses the scan but still returns ESP_OK with no networks
         * ("sta_scan: STA is connecting", found on the board). Stop the attempt and wait for the driver to say so; the
         * attempt resumes after the scan. */
        xEventGroupClearBits(s_ev, DISC_BIT);
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_ev, DISC_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
    }
    esp_err_t e = ESP_OK;
    uint16_t n = 0;
    for (int i = 0; i < 2; i++) { /* a second try when the first came back empty (the driver was still busy) */
        e = esp_wifi_scan_start(&sc, true);
        n = 0;
        if (e != ESP_OK || (esp_wifi_scan_get_ap_num(&n) == ESP_OK && n > 0)) break;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    s_scanning = false;
    if (e != ESP_OK) {
        NEMA_LOGW("wifi", "scan failed: %s", esp_err_to_name(e));
        resume_after_scan();
        return NEMA_ERR_IO;
    }
    wifi_ap_record_t *recs = (wifi_ap_record_t *)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(wifi_ap_record_t) * 24);
    if (!recs) {
        esp_wifi_clear_ap_list();
        resume_after_scan();
        return NEMA_ERR_NOMEM;
    }
    uint16_t want = n > 24 ? 24 : n;
    esp_wifi_scan_get_ap_records(&want, recs);
    size_t m = 0;
    for (uint16_t i = 0; i < want && m < max; i++) {
        nema_strlcpy(out[m].ssid, (const char *)recs[i].ssid, sizeof(out[m].ssid));
        out[m].rssi = recs[i].rssi;
        out[m].channel = recs[i].primary;
        out[m].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        m++;
    }
    nema_free(recs);
    resume_after_scan(); /* only now: a connect scans by itself and would replace the list before it was read */
    *found = m;
    return NEMA_OK;
}

static nema_err_t w_connect(const char *ssid, const char *pass, int32_t timeout_ms) {
    if (!s_started) return NEMA_ERR_STATE;
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    nema_strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    nema_strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_want = true;
    s_retry = 0;
    s_joining = true;
    if (s_reconnect) xTimerStop(s_reconnect, 0);
    xEventGroupClearBits(s_ev, GOT_IP_BIT | FAIL_BIT);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, GOT_IP_BIT | FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s_joining = false;
    if (b & GOT_IP_BIT) {
        nema_strlcpy(s_ssid, ssid, sizeof(s_ssid));
        nvs_save(ssid, pass);
        s_saved = true;
        return NEMA_OK;
    }
    s_want = false;
    esp_wifi_disconnect();
    return (b & FAIL_BIT) ? NEMA_ERR_PERM : NEMA_ERR_TIMEOUT;
}

static nema_err_t w_disconnect(void) {
    s_want = false;
    if (s_reconnect) xTimerStop(s_reconnect, 0);
    if (s_started) esp_wifi_disconnect();
    return NEMA_OK;
}

static nema_err_t w_forget(void) {
    w_disconnect();
    nvs_ready();
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_saved = false;
    return NEMA_OK;
}

static void w_status(pal_wifi_status_t *st) {
    st->connected = s_connected;
    st->saved = s_saved;
    st->time_synced = s_synced;
    nema_strlcpy(st->ssid, s_ssid, sizeof(st->ssid));
    nema_strlcpy(st->ip, s_ip, sizeof(st->ip));
    wifi_ap_record_t ap;
    st->rssi = (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

static const pal_wifi_ops_t WIFI_OPS = {w_scan, w_connect, w_disconnect, w_forget, w_status, w_has_credentials};

void pal_wifi_esp_register(void) { pal_wifi_set_ops(&WIFI_OPS); }

/* The time service: SNTP while the station has an address. Started and stopped by the supervisor. */
nema_err_t pal_wifi_esp_sntp_start(void) {
    if (s_sntp) return NEMA_OK;
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.sync_cb = sntp_synced;
    if (esp_netif_sntp_init(&sc) != ESP_OK) return NEMA_ERR_IO;
    s_sntp = true;
    return NEMA_OK;
}

void pal_wifi_esp_sntp_stop(void) {
    if (!s_sntp) return;
    esp_netif_sntp_deinit();
    s_sntp = false;
}

/* Undo everything a start did. Safe after a partial start: each step checks what it can. */
static void teardown(void) {
    s_want = false;
    if (s_reconnect) xTimerStop(s_reconnect, 0);
    s_backoff_s = 0;
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event);
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_sntp) {
        esp_netif_sntp_deinit();
        s_sntp = false;
    }
    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    s_connected = false;
    s_ip[0] = 0;
    s_started = false;
    pal_wifi_notify_ip(false);
}

nema_err_t pal_wifi_esp_start(void) {
    if (s_started) return NEMA_OK;
    nvs_ready();
    if (!s_base_up) {
        s_ev = xEventGroupCreate();
        s_reconnect = xTimerCreate("wifi-retry", pdMS_TO_TICKS(2000), pdFALSE, NULL, reconnect_due);
        esp_netif_init();
        esp_event_loop_create_default(); /* ESP_ERR_INVALID_STATE when another component made it: fine */
        s_base_up = true;
    }
    if (!s_ev) return NEMA_ERR_NOMEM;
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return NEMA_ERR_IO;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        NEMA_LOGE("wifi", "esp_wifi_init failed");
        esp_netif_destroy(s_netif);
        s_netif = NULL;
        return NEMA_ERR_IO;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    char ssid[33], pass[65];
    s_saved = nvs_load(ssid, sizeof(ssid), pass, sizeof(pass));
    if (s_saved) {
        wifi_config_t wc;
        memset(&wc, 0, sizeof(wc));
        nema_strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        nema_strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        nema_strlcpy(s_ssid, ssid, sizeof(s_ssid));
        s_want = true;
    }
    s_connected = false;
    s_ip[0] = 0;
    s_retry = 0;
    xEventGroupClearBits(s_ev, GOT_IP_BIT | FAIL_BIT);
    s_started = true;
    if (esp_wifi_start() != ESP_OK) {
        NEMA_LOGE("wifi", "esp_wifi_start failed");
        teardown();
        return NEMA_ERR_IO;
    }
    return NEMA_OK;
}

nema_err_t pal_wifi_esp_stop(void) {
    if (!s_started) return NEMA_OK;
    teardown();
    return NEMA_OK;
}
