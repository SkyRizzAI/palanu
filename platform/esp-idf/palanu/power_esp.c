/* Power profiles on the ESP32 (pal/power.h): dynamic frequency scaling through esp_pm (CONFIG_PM_ENABLE). The drivers hold their
 * own locks (SPI, UART, Wi-Fi), so a transfer runs at the speed it needs while the CPU idles low. Light sleep is esp_pm's automatic
 * one (CONFIG_FREERTOS_USE_TICKLESS_IDLE): the chip sleeps when every task waits and the next timer wakes it. */
#include "esp_pm.h"
#include "nema/nema.h"
#include "pal/power.h"
#include "sdkconfig.h"

static int s_min_mhz, s_max_mhz;
static bool s_light;

static nema_err_t configure(void) {
#if CONFIG_PM_ENABLE
    esp_pm_config_t c = {.max_freq_mhz = s_max_mhz, .min_freq_mhz = s_min_mhz,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
                         .light_sleep_enable = s_light
#else
                         .light_sleep_enable = false
#endif
    };
    esp_err_t e = esp_pm_configure(&c);
    if (e != ESP_OK) {
        NEMA_LOGW("power", "esp_pm_configure %d..%d MHz%s: %s", s_min_mhz, s_max_mhz, s_light ? " with light sleep" : "", esp_err_to_name(e));
        return NEMA_ERR_UNSUPPORTED;
    }
    return NEMA_OK;
#else
    return NEMA_ERR_UNSUPPORTED;
#endif
}

static nema_err_t light_sleep(bool allow) {
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    s_light = allow;
    nema_err_t r = configure();
    if (r == NEMA_OK) NEMA_LOGI("power", "light sleep %s", allow ? "allowed (screen off)" : "off");
    return r;
#else
    (void)allow;
    return NEMA_ERR_UNSUPPORTED;
#endif
}

static nema_err_t cpu(int min_mhz, int max_mhz) {
    s_min_mhz = min_mhz, s_max_mhz = max_mhz, s_light = false;
    nema_err_t r = configure();
    if (r == NEMA_OK) NEMA_LOGI("power", "CPU %d..%d MHz", min_mhz, max_mhz);
    return r;
}

void pal_power_esp_register(void) {
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    static const pal_power_ops_t OPS = {.cpu = cpu, .max_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, .light_sleep = light_sleep};
#else
    /* no tickless idle in this build (sdkconfig.defaults says why): the Saver profile does not offer light sleep */
    static const pal_power_ops_t OPS = {.cpu = cpu, .max_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ};
    (void)light_sleep;
#endif
    pal_power_set_ops(&OPS);
}
