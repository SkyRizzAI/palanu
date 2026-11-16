/* The notify module of the palanu API (api/palanu/notify.h). The rules (ring, toast queue, rate limit) are in
 * system/src/notify.c; this file names the posting app. The glue checks the notify capability before these run. */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "pal/notify.h"

/* The posting app's id; native callers (no context) are "system". */
static const char *source_of(api_ctx_t *c) { return c ? api_app_id(c) : "system"; }

int32_t psys_api_toast(api_ctx_t *c, const char *text, int32_t ms) { return psys_toast_post(source_of(c), text, ms); }

int32_t psys_api_notify_post(api_ctx_t *c, const char *title, const char *text, int32_t flags) {
    return psys_notify_post(source_of(c), title, text, flags);
}

int32_t psys_api_notify_update(api_ctx_t *c, int32_t id, const char *title, const char *text) {
    return psys_notify_update(source_of(c), id, title, text);
}

int32_t psys_api_notify_cancel(api_ctx_t *c, int32_t id) { return psys_notify_cancel(source_of(c), id); }
