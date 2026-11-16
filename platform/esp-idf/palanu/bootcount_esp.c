/* The boot-attempt counter in NVS. One u32 in namespace "sys". NVS is brought up here if nothing else has done it
 * yet (the Wi-Fi driver does the same, and a second init is reported as already done). Untested on the board. */
#include "nvs.h"
#include "nvs_flash.h"
#include "pal/bootcount.h"
#include "pal_services_esp.h"

#define NS "sys"
#define KEY "bootcnt"

static bool s_nvs_ready;

static void nvs_up(void) {
    if (s_nvs_ready) return;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    s_nvs_ready = e == ESP_OK || e == ESP_ERR_INVALID_STATE;
}

static uint32_t bc_get(void) {
    nvs_up();
    nvs_handle_t h;
    if (!s_nvs_ready || nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t n = 0;
    if (nvs_get_u32(h, KEY, &n) != ESP_OK) n = 0; /* not stored yet */
    nvs_close(h);
    return n;
}

static nema_err_t bc_set(uint32_t n) {
    nvs_up();
    nvs_handle_t h;
    if (!s_nvs_ready || nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return NEMA_ERR_IO;
    esp_err_t e = nvs_set_u32(h, KEY, n);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? NEMA_OK : NEMA_ERR_IO;
}

static const pal_bootcount_ops_t BOOTCOUNT = {bc_get, bc_set};

const pal_bootcount_ops_t *pal_bootcount_esp(void) { return &BOOTCOUNT; }
