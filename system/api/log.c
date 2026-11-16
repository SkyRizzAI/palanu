/* The log module of the palanu API (api/palanu/log.h): one line to the system log, tagged with the app's id. */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"

/* The levels are the values of PAL_LOG_* in api/palanu/log.h (0 error, 1 warn, 2 info, 3 debug). */
int32_t psys_api_log(api_ctx_t *c, int32_t level, const char *msg) {
    nema_log_level_t l;
    switch (level) {
    case 0: l = NEMA_LOG_ERROR; break;
    case 1: l = NEMA_LOG_WARN; break;
    case 2: l = NEMA_LOG_INFO; break;
    case 3: l = NEMA_LOG_DEBUG; break;
    default: return NEMA_ERR_INVAL;
    }
    nema_log(l, api_app_id(c), "%s", msg);
    return NEMA_OK;
}
