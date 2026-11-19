/* The device as a Bluetooth LE keyboard (pal/hid.h, ADR-0018): HID over GATT with NimBLE. Three services: HID (the report map of a
 * keyboard, one input report), Battery and Device Information (the PnP ID HOGP asks for). The host pairs with Just Works and
 * bonds; the bond is kept in NVS, so a paired computer reconnects after a restart.
 *
 * While it runs it has the radio to itself: Palanu Link over BLE is stopped at start and started again at stop. */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nema/nema.h"
#include "pal/hid.h"
#include "pal_radio_esp.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void); /* NimBLE's bond store (store/config); its header is not on the component's include path */

static const char *TAG = "hid-ble";

#define APPEARANCE_KEYBOARD 0x03C1
#define BLE_REASON_USER_TERM 0x13

/* A keyboard: report 1 is the modifiers, a reserved byte and six keys (USB HID usage page 7). */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /*   Report ID (1) */
    0x05, 0x07,       /*   Usage Page (Keyboard) */
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, /* modifiers: 8 bits */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                                 /* reserved byte */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, /* six keys */
    0xC0,             /* End Collection */
};
static const uint8_t HID_INFO[4] = {0x11, 0x01, 0x00, 0x02}; /* HID 1.11, no country, normally connectable */
static const uint8_t PNP_ID[7] = {0x02, 0x3A, 0x30, 0x01, 0x80, 0x00, 0x01}; /* USB vendor space, 0x303A, product 0x8001, 1.0 */
static const uint8_t REPORT_REF_INPUT[2] = {0x01, 0x01};                    /* report 1, input */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_input_handle;
static bool s_notify, s_encrypted, s_started, s_stopping, s_link_was_on;
static uint8_t s_addr_type, s_protocol = 1, s_battery = 100;
static uint8_t s_input[8];

static void advertise(void);

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    uint16_t uuid = ble_uuid_u16(ctxt->chr ? ctxt->chr->uuid : ctxt->dsc->uuid);
    const void *val = NULL;
    size_t len = 0;
    switch (uuid) {
    case 0x2A4B: val = REPORT_MAP, len = sizeof(REPORT_MAP); break;
    case 0x2A4A: val = HID_INFO, len = sizeof(HID_INFO); break;
    case 0x2A50: val = PNP_ID, len = sizeof(PNP_ID); break;
    case 0x2A4D: val = s_input, len = sizeof(s_input); break;
    case 0x2908: val = REPORT_REF_INPUT, len = sizeof(REPORT_REF_INPUT); break;
    case 0x2A19: val = &s_battery, len = 1; break;
    case 0x2A4E: /* protocol mode: report (1) or boot (0); read and written */
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            if (OS_MBUF_PKTLEN(ctxt->om) == 1) os_mbuf_copydata(ctxt->om, 0, 1, &s_protocol);
            return 0;
        }
        val = &s_protocol, len = 1;
        break;
    case 0x2A4C: return 0; /* control point: suspend or exit suspend, nothing to do */
    default: return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR && ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, val, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#define RD (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
static const struct ble_gatt_svc_def GATT[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x1812),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = chr_access, .flags = RD},
         {.uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = chr_access, .flags = RD},
         {.uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
         {.uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = chr_access,
          .flags = RD | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
         {.uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = chr_access, .val_handle = &s_input_handle,
          .flags = RD | BLE_GATT_CHR_F_NOTIFY,
          .descriptors = (struct ble_gatt_dsc_def[]){
              {.uuid = BLE_UUID16_DECLARE(0x2908), .access_cb = chr_access, .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC}, {0}}},
         {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x180F),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY}, {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(0x180A),
     .characteristics = (struct ble_gatt_chr_def[]){{.uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ},
                                                    {0}}},
    {0}};
#undef RD

static int gap_event(struct ble_gap_event *e, void *arg) {
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s_conn = e->connect.conn_handle;
            s_encrypted = s_notify = false;
            ble_gap_security_initiate(s_conn); /* a bonded host encrypts again at once; a new one is asked to pair */
            ESP_LOGI(TAG, "connected");
        } else if (!s_stopping) advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_encrypted = s_notify = false;
        ESP_LOGI(TAG, "disconnected (%d)", e->disconnect.reason);
        if (!s_stopping) advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_encrypted = e->enc_change.status == 0;
        ESP_LOGI(TAG, "encryption %s", s_encrypted ? "on" : "failed");
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == s_input_handle) s_notify = e->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: { /* the host lost its bond and pairs again: forget the old one */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(e->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
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
    f.appearance = APPEARANCE_KEYBOARD;
    f.appearance_is_present = 1;
    f.uuids16 = (ble_uuid16_t[]){BLE_UUID16_INIT(0x1812)};
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    if (ble_gap_adv_set_fields(&f)) return;
    struct ble_hs_adv_fields r = {0};
    r.name = (const uint8_t *)name;
    r.name_len = (uint8_t)strlen(name);
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "advertise: %d", rc);
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
}

static void host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static nema_err_t hid_start(const char *name) {
    if (s_started) return NEMA_OK;
    s_link_was_on = pal_link_ble_running();
    pal_link_ble_stop(); /* one GATT server at a time: Link comes back at stop */
    if (nimble_port_init() != ESP_OK) {
        if (s_link_was_on) (void)pal_link_ble_start("PalanuOS");
        return NEMA_ERR_IO;
    }
    s_stopping = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_notify = s_encrypted = false;
    memset(s_input, 0, sizeof(s_input));
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; /* Just Works: the device has no way to show or enter a code to the host */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    (void)ble_gatts_reset();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(name);
    ble_svc_gap_device_appearance_set(APPEARANCE_KEYBOARD);
    if (ble_gatts_count_cfg(GATT) || ble_gatts_add_svcs(GATT)) {
        ESP_LOGW(TAG, "gatt setup failed");
        nimble_port_deinit();
        if (s_link_was_on) (void)pal_link_ble_start("PalanuOS");
        return NEMA_ERR_IO;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    s_started = true;
    return NEMA_OK;
}

static void hid_stop(void) {
    if (!s_started) return;
    s_stopping = true;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_REASON_USER_TERM);
    ble_gap_adv_stop();
    for (int i = 0; i < 100 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) nema_sleep_ms(10);
    if (nimble_port_stop()) ESP_LOGW(TAG, "host stop failed");
    nema_sleep_ms(50);
    if (nimble_port_deinit() != ESP_OK) ESP_LOGW(TAG, "controller deinit failed");
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_started = false;
    if (s_link_was_on) (void)pal_link_ble_start("PalanuOS");
}

static pal_hid_state_t hid_state(void) {
    if (!s_started) return PAL_HID_OFF;
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_encrypted && s_notify ? PAL_HID_CONNECTED : PAL_HID_WAITING;
}

static nema_err_t hid_report(uint8_t mods, const uint8_t keys[6]) {
    if (hid_state() != PAL_HID_CONNECTED) return NEMA_ERR_STATE;
    s_input[0] = mods;
    s_input[1] = 0;
    memcpy(s_input + 2, keys, 6);
    for (int tries = 0; tries < 50; tries++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_input, sizeof(s_input));
        if (!om) {
            nema_sleep_ms(5);
            continue;
        }
        int rc = ble_gatts_notify_custom(s_conn, s_input_handle, om);
        if (rc == 0) return NEMA_OK;
        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) return NEMA_ERR_IO;
        nema_sleep_ms(5); /* the controller's queue is full: let it drain */
    }
    return NEMA_ERR_TIMEOUT;
}

static void hid_peer(char *out, size_t cap) {
    struct ble_gap_conn_desc d;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(s_conn, &d) != 0) {
        snprintf(out, cap, "%s", "");
        return;
    }
    const uint8_t *a = d.peer_id_addr.val;
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static const pal_hid_ops_t OPS = {hid_start, hid_stop, hid_state, hid_report, hid_peer};

void pal_hid_ble_register(void) { pal_hid_set(&OPS); }
