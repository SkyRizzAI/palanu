/* Power profiles on the ESP32 (pal/power.h): dynamic frequency scaling through esp_pm (CONFIG_PM_ENABLE). The drivers hold their
 * own locks (SPI, UART, Wi-Fi), so a transfer runs at the speed it needs while the CPU idles low. No light sleep (power.h). */
#include "esp_pm.h"
#include "nema/nema.h"
#include "pal/power.h"
#include "sdkconfig.h"

static nema_err_t cpu(int min_mhz, int max_mhz) {
#if CONFIG_PM_ENABLE
    esp_pm_config_t c = {.max_freq_mhz = max_mhz, .min_freq_mhz = min_mhz, .light_sleep_enable = false};
    esp_err_t e = esp_pm_configure(&c);
    if (e != ESP_OK) {
        NEMA_LOGW("power", "esp_pm_configure %d..%d MHz: %s", min_mhz, max_mhz, esp_err_to_name(e));
        return NEMA_ERR_UNSUPPORTED;
    }
    NEMA_LOGI("power", "CPU %d..%d MHz", min_mhz, max_mhz);
    return NEMA_OK;
#else
    (void)min_mhz, (void)max_mhz;
    return NEMA_ERR_UNSUPPORTED;
#endif
}

void pal_power_esp_register(void) {
    static const pal_power_ops_t OPS = {.cpu = cpu, .max_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ};
    pal_power_set_ops(&OPS);
}
