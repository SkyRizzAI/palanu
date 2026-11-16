/* The system settings as the palanu settings API reads and changes them (api/palanu/settings.h). The table of keys is
 * in system/src/settings_api.c. Some values belong to the built-in apps: the terminal font, the radio policy, the device
 * name, and the file they are saved in. The app side binds them once at boot with psys_settings_bind; until then those
 * keys answer PAL_ERR_NOENT. */
#ifndef PAL_SETTINGS_H
#define PAL_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int (*font_get)(void);                      /* 0 to 2 */
    void (*font_set)(int font);                 /* applies at once */
    const char *(*radio_get)(const char *key);  /* "radio.wifi" (auto|off|on) or "radio.ble" (off|on) */
    const char *(*name_get)(void);              /* the device name */
    int (*name_set)(const char *name);          /* 0, or a negative error; saves it */
    void (*save)(void);                         /* saves the settings file after a change */
} psys_settings_host_t;

void psys_settings_bind(const psys_settings_host_t *host);

/* Values with no owning module (system/src/settings_kv.c): a bool, an integer, a named choice or a short text. They are
 * kept in /data/settings with the rest and changed through psys_settings_set like every key. A module reads them here. */
int32_t psys_settings_int(const char *key, int32_t def); /* an integer, bool (0/1) or choice index; def when unknown */
const char *psys_settings_text(const char *key);         /* a text key, or a choice's name; "" when unknown */
/* The boot loader and the saver (settings_store.c): one line each, and the effects after the load. */
void psys_settings_kv_load(const char *key, const char *value);
size_t psys_settings_kv_format(char *out, size_t cap);   /* "key=value\n" lines; 0 when cap is too small */
void psys_settings_kv_apply(void);
bool psys_settings_kv_has(const char *key);
int psys_settings_kv_get(const char *key, char *out, size_t cap);
int psys_settings_kv_set(const char *key, const char *value);
/* The settings table (system/src/settings_api.c): every change of a key goes through set; both return 0 or a negative
 * nema_err_t. */
int psys_settings_get(const char *key, char *out, size_t cap);
int psys_settings_set(const char *key, const char *value);

/* Called after each change that applied: with the key's name. One callback; a new one replaces the old. The UI and the
 * events (PAL_EV_SETTING, later) use it. */
typedef void (*psys_settings_cb_t)(const char *key);
void psys_settings_on_change(psys_settings_cb_t cb);

#endif
