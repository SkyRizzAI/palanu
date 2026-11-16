/* Settings background jobs: the operations that block (the radio starts, a Wi-Fi scan or connect, removing an app) run on one
 * worker task, so the desktop never waits on them. A page starts a job; the view's pump takes the result on the desktop
 * task and shows it (busy overlay, toast, refreshed rows). One job at a time: the caller checks settings_job_busy first. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "nema/nema.h"
#include "nema/task.h"
#include "pal/sd.h"
#include "pal/papp.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/wifi.h"
#include "settings_int.h"

#define JOB_STACK 8192u /* the radio ops use 8 KB as well; this task runs the same kind of calls */
#define SCAN_MAX 24
#define TICK_MS 10

static struct {
    bool started;
    volatile bool pending;  /* a job was started and the worker has not run it yet */
    volatile bool ready;    /* the worker finished; the desktop has not taken it yet */
    settings_job_t kind;
    char a[65], b[65];      /* arguments in, and the error text out (in a) */
    int rc;
    pal_ap_t aps[SCAN_MAX];
    size_t naps;
    bool scanned;
} J NEMA_EXT_BSS;

static const char *const NAMES[] = {"", "Wi-Fi on", "Wi-Fi off", "Bluetooth on", "Bluetooth off", "Scan", "Connect",
                                    "Disconnect", "Forget", "Start", "Stop", "Restart", "Uninstall", "Mount card", "Eject card", "Remote on", "Remote off"};

const char *settings_job_name(settings_job_t k) { return (unsigned)k < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[k] : ""; }

static void set_error(const char *text) { snprintf(J.a, sizeof(J.a), "%s", text ? text : ""); }

/* The radio policy at boot (radio.wifi, radio.ble), kept even when the start is refused, so the next boot tries again. */
static void radio_policy(const char *name, const char *value) { pal_settings_set_radio(name, value); }

/* a holds the job's argument (the SSID, the service id, the app id); a failure may overwrite it with the reason. */
static void run(settings_job_t k) {
    J.rc = NEMA_OK;
    switch (k) {
    case JOB_WIFI_ON:
        radio_policy("radio.wifi", "on");
        (void)psys_svc_start("radio-wifi"); /* the service holds the radio; a refusal retries every 5 s */
        if (pal_radio_state(PAL_RADIO_WIFI) != PAL_RADIO_ON) {
            J.rc = NEMA_ERR_IO;
            set_error(pal_radio_last_error(PAL_RADIO_WIFI));
        }
        break;
    case JOB_WIFI_OFF:
        (void)psys_svc_stop("radio-wifi"); /* releases the radio and records radio.wifi=off */
        break;
    case JOB_BLE_ON:
        radio_policy("radio.ble", "on");
        (void)psys_svc_start("radio-ble");
        if (pal_radio_state(PAL_RADIO_BLE) != PAL_RADIO_ON) {
            J.rc = NEMA_ERR_IO;
            set_error(pal_radio_last_error(PAL_RADIO_BLE));
        }
        break;
    case JOB_BLE_OFF:
        (void)psys_svc_stop("link-ble"); /* the link holds the radio too */
        (void)psys_svc_stop("radio-ble");
        break;
    case JOB_SCAN:
        if (pal_radio_state(PAL_RADIO_WIFI) != PAL_RADIO_ON) {
            J.rc = NEMA_ERR_STATE;
            set_error("Turn Wi-Fi on first");
            break;
        }
        J.naps = 0;
        J.rc = pal_wifi_scan(J.aps, SCAN_MAX, &J.naps);
        J.scanned = true;
        break;
    case JOB_CONNECT:
        J.rc = pal_wifi_connect(J.a, J.b, 20000);
        memset(J.b, 0, sizeof(J.b));
        break;
    case JOB_DISCONNECT:
        J.rc = pal_wifi_disconnect();
        break;
    case JOB_FORGET:
        J.rc = pal_wifi_forget();
        break;
    case JOB_SVC_START:
        J.rc = psys_svc_start(J.a);
        break;
    case JOB_SVC_STOP:
        J.rc = psys_svc_stop(J.a);
        break;
    case JOB_SVC_RESTART:
        J.rc = psys_svc_restart(J.a);
        break;
    case JOB_UNINSTALL:
        J.rc = pal_papp_remove(J.a);
        break;
    case JOB_REMOTE_ON: /* both servers; the first refusal is the answer */
        J.rc = psys_svc_start("link-tcp");
        if (psys_svc_start("link-ws") != NEMA_OK && J.rc == NEMA_OK) J.rc = NEMA_ERR_IO;
        break;
    case JOB_REMOTE_OFF:
        J.rc = psys_svc_stop("link-tcp");
        (void)psys_svc_stop("link-ws");
        break;
    case JOB_SD_MOUNT:
        J.rc = pal_sd_mount();
        break;
    case JOB_SD_EJECT:
        J.rc = pal_sd_eject();
        break;
    default:
        J.rc = NEMA_ERR_INVAL;
        break;
    }
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        if (J.pending) {
            run(J.kind);
            J.pending = false;
            J.ready = true;
        }
        nema_sleep_ms(TICK_MS);
    }
}

bool settings_job_busy(void) { return J.pending || J.ready; }

bool settings_job_start(settings_job_t k, const char *a, const char *b) {
    if (settings_job_busy() || k == JOB_NONE) return false;
    if (!J.started) {
        nema_task_desc_t td = {.name = "settings-job", .fn = worker, .arg = NULL, .stack = JOB_STACK, .prio = 2,
                               .core = -1, .role = NEMA_TASK_SERVICE, .owner = nema_owner_register("settings")};
        if (nema_task_create(&td) != NEMA_OK) return false;
        J.started = true;
    }
    J.kind = k;
    snprintf(J.a, sizeof(J.a), "%s", a ? a : "");
    snprintf(J.b, sizeof(J.b), "%s", b ? b : "");
    J.pending = true;
    return true;
}

bool settings_job_take(settings_job_t *k, int *rc, char *arg, size_t cap) {
    if (!J.ready) return false;
    *k = J.kind;
    *rc = J.rc;
    if (arg && cap) snprintf(arg, cap, "%s", J.a);
    J.ready = false;
    return true;
}

size_t settings_scan_get(pal_ap_t *out, size_t max) {
    size_t n = J.naps < max ? J.naps : max;
    if (out) memcpy(out, J.aps, n * sizeof(pal_ap_t));
    return n;
}

bool settings_scan_done(void) { return J.scanned; }
