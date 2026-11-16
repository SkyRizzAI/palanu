/* Firmware update on the board: write the inactive slot, verify, switch. With the bootloader's rollback enabled, a new
 * image must prove itself: it stays "pending verification" until the health check (system/src/ota_confirm.c)
 * confirms it. A reboot before that makes the bootloader return to the previous image. */
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "pal/ota.h"
#include "pal/ota_confirm.h"
#include "pal_console.h"

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;

static nema_err_t map(esp_err_t e) {
    switch (e) {
    case ESP_OK: return NEMA_OK;
    case ESP_ERR_NO_MEM: return NEMA_ERR_NOMEM;
    case ESP_ERR_INVALID_SIZE: return NEMA_ERR_RANGE;
    case ESP_ERR_OTA_VALIDATE_FAILED: return NEMA_ERR_PROTO; /* not a valid image for this chip */
    default: return NEMA_ERR_IO;
    }
}

static nema_err_t ota_begin(size_t size) {
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!s_target) return NEMA_ERR_NOENT;
    if (size > s_target->size) return NEMA_ERR_NOSPC;
    return map(esp_ota_begin(s_target, size ? size : OTA_SIZE_UNKNOWN, &s_handle));
}

static nema_err_t ota_write(const void *d, size_t n) { return map(esp_ota_write(s_handle, d, n)); }

static nema_err_t ota_end(void) {
    nema_err_t r = map(esp_ota_end(s_handle));
    if (r == NEMA_OK) r = map(esp_ota_set_boot_partition(s_target));
    return r;
}

static void ota_abort(void) { esp_ota_abort(s_handle); }

static const char *ota_running(void) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "";
}

static const char *ota_version(void) { return esp_app_get_description()->version; }

static const pal_ota_ops_t OPS = {ota_begin, ota_write, ota_end, ota_abort, ota_running, ota_version};

/* The health check's ops. Reboot only for an image that is pending verification: a board flashed over USB is never
 * rebooted by the check (it has no update to roll back). */
static bool confirm_pending(void) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static bool confirm_image(void) { return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK; }

static void reboot_board(void) {
    if (confirm_pending()) esp_restart();
}

static const pal_ota_confirm_ops_t CONFIRM = {confirm_pending, confirm_image, reboot_board};

/* Called from pal_esp_register_services. The 10-second timer that confirmed every image is gone: the ota-confirm service
 * confirms after its health check. */
void pal_ota_esp_start(void) {
    pal_ota_set(&OPS);
    psys_ota_confirm_set_ops(&CONFIRM);
}
