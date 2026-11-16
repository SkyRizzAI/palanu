/* OTA confirmation policy and its task (system/include/pal/ota_confirm.h). The policy is a pure function of the
 * health and the elapsed time; the task only reads the health, asks the policy, and calls the platform's ops. */
#include "pal/ota_confirm.h"

#include <stdio.h>
#include <string.h>

#include "pal/desktop.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/sysinfo.h"

#define CHECK_STACK 3072u

static const pal_ota_confirm_ops_t *s_ops;
static bool s_started;

psys_ota_action_t psys_ota_decide(const psys_ota_health_t *h, uint32_t elapsed_ms, char *why, size_t cap) {
    if (h->desktop_frame && h->link_up && h->radios_ok) return PSYS_OTA_CONFIRM;
    if (elapsed_ms < PSYS_OTA_WINDOW_MS) return PSYS_OTA_WAIT;
    if (why && cap) {
        why[0] = 0;
        if (!h->desktop_frame) strncat(why, "no desktop frame; ", cap - strlen(why) - 1);
        if (!h->link_up) strncat(why, "link-uart not running; ", cap - strlen(why) - 1);
        if (!h->radios_ok) strncat(why, "a radio failed; ", cap - strlen(why) - 1);
    }
    return PSYS_OTA_ROLLBACK;
}

psys_ota_health_t psys_ota_health_read(void) {
    psys_ota_health_t h;
    psys_svc_info_t link;
    h.desktop_frame = pal_desktop_frames() > 0;
    h.link_up = psys_svc_find("link-uart", &link) && link.state == PSYS_SVC_RUNNING;
    h.radios_ok = pal_radio_state(PAL_RADIO_WIFI) != PAL_RADIO_FAILED && pal_radio_state(PAL_RADIO_BLE) != PAL_RADIO_FAILED;
    return h;
}

void psys_ota_confirm_set_ops(const pal_ota_confirm_ops_t *ops) { s_ops = ops; }

static void check_task(void *arg) {
    (void)arg;
    uint64_t t0 = pal_sys_uptime_us();
    bool pending = s_ops && s_ops->pending ? s_ops->pending() : false;
    if (!pending) NEMA_LOGI("ota", "running image is not awaiting confirmation; no health check needed");
    for (;;) {
        uint32_t elapsed = (uint32_t)((pal_sys_uptime_us() - t0) / 1000u);
        char why[96];
        psys_ota_health_t h = psys_ota_health_read();
        psys_ota_action_t a = psys_ota_decide(&h, elapsed, why, sizeof(why));
        if (!pending) return;
        if (a == PSYS_OTA_WAIT) {
            nema_sleep_ms(PSYS_OTA_PERIOD_MS);
            continue;
        }
        if (a == PSYS_OTA_CONFIRM) {
            if (s_ops->confirm && s_ops->confirm()) NEMA_LOGI("ota", "image confirmed: desktop, link-uart and radios are healthy");
            else NEMA_LOGW("ota", "image healthy but confirmation failed");
            return;
        }
        NEMA_LOGE("ota", "health check failed after %lu s: %s; not confirmed, rebooting to roll back",
                  (unsigned long)(PSYS_OTA_WINDOW_MS / 1000u), why);
        if (s_ops->reboot) s_ops->reboot();
        return;
    }
}

nema_err_t psys_ota_confirm_start(void) {
    if (s_started) return NEMA_OK;
    nema_task_desc_t td = {.name = "ota-check", .fn = check_task, .arg = NULL, .stack = CHECK_STACK, .prio = 2, .core = -1,
                           .role = NEMA_TASK_SYSTEM, .owner = nema_owner_register("ota")};
    nema_err_t r = nema_task_create(&td);
    if (r == NEMA_OK) s_started = true;
    return r;
}
