/* Palanu Link over Bluetooth LE (docs/PLP.md, "BLE"). One GATT service with two characteristics:
 *   rx  (write without response): the client writes PLP bytes here
 *   tx  (notify):                 the device sends PLP bytes here, split to the negotiated MTU
 * The byte stream carries the same frames as UART and TCP; nothing above the transport knows the difference.
 *
 * The radio manager starts and stops it. Start brings the controller and the host up and advertises; stop disconnects
 * a client, stops the host, and deinitialises the controller, so its RAM goes back to the heap (docs/RADIO.md). */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nema/nema.h"
#include "pal/link.h"
#include "pal_radio_esp.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "link-ble";

/* Service 6f1d0001-..., rx 6f1d0002-..., tx 6f1d0003-... (random, generic). Little-endian byte order. */
#define UUID128(n) BLE_UUID128_INIT(0x4e, 0x9a, 0x1c, 0x37, 0xd1, 0x52, 0x8b, 0x8f, 0x1f, 0x4c, 0x7a, 0x3b, (n), 0x00, 0x1d, 0x6f)
static const ble_uuid128_t SVC_UUID = UUID128(0x01);
static const ble_uuid128_t RX_UUID = UUID128(0x02);
static const ble_uuid128_t TX_UUID = UUID128(0x03);

/* BLE_ERR_REM_USER_CONN_TERM: the HCI reason "remote user terminated connection". */
#define BLE_REASON_USER_TERM 0x13

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static bool s_notify;
static pal_link_t *s_link;
static uint8_t s_addr_type;
static bool s_started;  /* controller and host are up */
static bool s_stopping; /* stop is in progress: do not advertise again */
static char s_name[32];

static void advertise(void);

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !s_link) return BLE_ATT_ERR_UNLIKELY;
    for (struct os_mbuf *m = ctxt->om; m; m = SLIST_NEXT(m, om_next))
        if (m->om_len) pal_link_feed(s_link, m->om_data, m->om_len);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) { return BLE_ATT_ERR_READ_NOT_PERMITTED; }

static const struct ble_gatt_svc_def GATT[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &SVC_UUID.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &RX_UUID.u, .access_cb = rx_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE},
         {.uuid = &TX_UUID.u, .access_cb = tx_access, .val_handle = &s_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY},
         {0}}},
    {0}};

static int link_tx(void *ctx, const uint8_t *d, size_t n) {
    while (n) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify) return -1;
        uint16_t mtu = ble_att_mtu(s_conn);
        size_t chunk = mtu > 3 ? mtu - 3u : 20u;
        if (chunk > n) chunk = n;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)chunk);
        if (!om) {
            nema_sleep_ms(5);
            continue;
        }
        int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) { /* the controller's queue is full: wait for it to drain */
            nema_sleep_ms(5);
            continue;
        }
        if (rc) return -1;
        d += chunk;
        n -= chunk;
    }
    return 0;
}

static int gap_event(struct ble_gap_event *e, void *arg) {
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s_conn = e->connect.conn_handle;
            pal_link_io_t io = {.tx = link_tx, .transport = PAL_LINK_BLE}; /* untrusted until AUTH (docs/PLP.md) */
            s_link = pal_link_create(&io);
            ESP_LOGI(TAG, "connected");
        } else if (!s_stopping) advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        pal_link_t *l = s_link;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify = false;
        s_link = NULL;
        if (l) pal_link_destroy(l);
        ESP_LOGI(TAG, "disconnected");
        if (!s_stopping) advertise();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == s_tx_handle) s_notify = e->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %d", e->mtu.value);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_stopping) advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    if (s_stopping) return;
    struct ble_hs_adv_fields f = {0};
    const char *name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)name;
    f.name_len = (uint8_t)strlen(name);
    f.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&f)) return;
    struct ble_hs_adv_fields r = {0};
    r.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    r.num_uuids128 = 1;
    r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "advertise: %d", rc);
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising");
}

static void host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

nema_err_t pal_link_ble_start(const char *name) {
    if (s_started) return NEMA_OK;
    nema_strlcpy(s_name, name, sizeof(s_name));
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGW(TAG, "controller init failed");
        return NEMA_ERR_IO;
    }
    s_stopping = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_notify = false;
    s_link = NULL;
    ble_hs_cfg.sync_cb = on_sync;
    (void)ble_gatts_reset(); /* a restart must not keep the previous service table */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_name);
    if (ble_gatts_count_cfg(GATT) || ble_gatts_add_svcs(GATT)) {
        ESP_LOGW(TAG, "gatt setup failed");
        nimble_port_deinit();
        return NEMA_ERR_IO;
    }
    nimble_port_freertos_init(host_task);
    s_started = true;
    return NEMA_OK;
}

void pal_link_ble_stop(void) {
    if (!s_started) return;
    s_stopping = true;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_REASON_USER_TERM);
    ble_gap_adv_stop();
    for (int i = 0; i < 100 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) nema_sleep_ms(10); /* disconnect is processed by the host task */
    int rc = nimble_port_stop();
    if (rc) ESP_LOGW(TAG, "host stop: %d", rc);
    nema_sleep_ms(50); /* the host task deletes itself after nimble_port_run() returns */
    if (nimble_port_deinit() != ESP_OK) ESP_LOGW(TAG, "controller deinit failed");
    pal_link_t *l = s_link;
    s_link = NULL;
    if (l) pal_link_destroy(l);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_notify = false;
    s_started = false;
}

bool pal_link_ble_running(void) { return s_started; }

size_t pal_link_ble_status(char *out, size_t cap) {
    int n;
    if (!s_started) n = snprintf(out, cap, "not connected (radio off)");
    else if (s_conn != BLE_HS_CONN_HANDLE_NONE) n = snprintf(out, cap, "connected to a client (device \"%s\")", s_name);
    else n = snprintf(out, cap, "not connected, advertising as \"%s\"", s_name);
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
