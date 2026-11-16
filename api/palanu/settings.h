/* System settings (API level 2). A setting is a text key with a text value: ui.dark (0|1), ui.theme (index or name), ui.size
 * (0 small, 1 medium), time.tz (minutes from UTC), display.brightness (0 to 100; the panel keeps at least 5), term.font
 * (0 to 2), device.name, and the radio policy radio.wifi and radio.ble (read only: the wifi and ble commands change them).
 * Reading needs the settings.read permission and changing a value needs settings.write. A change applies at once and is
 * kept across restarts. Unknown keys give PAL_ERR_NOENT; a read-only key gives PAL_ERR_PERM. */
#ifndef PALANU_API_SETTINGS_H
#define PALANU_API_SETTINGS_H

#include "annot.h"

PAL_SINCE(2) PAL_CAP("settings.read") PAL_WASM(setting_get)
PAL_DOC("Copies the value of key, as text, into buf with a NUL after it. Returns the length without the NUL, PAL_ERR_NOENT for an unknown key, or PAL_ERR_RANGE when cap cannot hold the value and its NUL.")
int32_t pal_setting_get(const char *key, PAL_OUT(cap) void *buf, uint32_t cap);

PAL_SINCE(2) PAL_CAP("settings.write") PAL_WASM(setting_set)
PAL_DOC("Sets key to the text value, checks it, applies it at once, and saves it. Returns 0, PAL_ERR_NOENT (unknown key), PAL_ERR_PERM (read-only key), or PAL_ERR_INVAL (value out of range or malformed).")
int32_t pal_setting_set(const char *key, const char *value);

#endif
