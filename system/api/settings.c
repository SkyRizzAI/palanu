/* The settings module of the palanu API (api/palanu/settings.h). The keys, their ranges and their effects are in
 * system/src/settings_api.c; this file is the call the glue makes. The capability (settings.read, settings.write) is
 * checked by the glue before these run. */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "../src/settings_api.h"

int32_t psys_api_setting_get(api_ctx_t *c, const char *key, void *buf, uint32_t cap) {
    (void)c;
    return psys_settings_get(key, (char *)buf, cap);
}

int32_t psys_api_setting_set(api_ctx_t *c, const char *key, const char *value) {
    (void)c;
    return psys_settings_set(key, value);
}
