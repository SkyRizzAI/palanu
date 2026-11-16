/* Board side of the radio manager (pal/radio.h): Wi-Fi and BLE bring-up and teardown, and the glue that registers
 * them. Everything here is ESP-IDF; portable code never sees it. */
#ifndef PAL_RADIO_ESP_H
#define PAL_RADIO_ESP_H

#include "nema/types.h"

/* Register the radio ops, the Wi-Fi ops (for the wifi command) and the Wi-Fi address hook. Call before pal_boot(). */
void pal_radio_esp_init(void);

/* Wi-Fi station: full bring-up (netif, driver, saved credentials, SNTP on address) and full teardown. Blocking. */
nema_err_t pal_wifi_esp_start(void);
nema_err_t pal_wifi_esp_stop(void);
void pal_wifi_esp_register(void); /* the Wi-Fi ops, with has_credentials; no driver is started */

/* Palanu Link over BLE: controller and host bring-up with the GATT service, and teardown. Blocking. */
nema_err_t pal_link_ble_start(const char *name);
void pal_link_ble_stop(void);
size_t pal_link_ble_status(char *out, size_t cap); /* one line; "not connected ..." when no client is linked */

/* The time service: SNTP while the station has an address. Started and stopped by the supervisor. */
nema_err_t pal_wifi_esp_sntp_start(void);
void pal_wifi_esp_sntp_stop(void);

#endif
