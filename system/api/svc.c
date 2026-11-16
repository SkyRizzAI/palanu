/* The svc module of the palanu API (api/palanu/svc.h): pal_svc_ready. The supervisor finds the calling service by its process
 * and marks it ready (system/src/svc.c). The NEMA error codes are the PAL codes, so they are returned as they are. */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"
#include "pal/svc.h"

int32_t psys_api_svc_ready(api_ctx_t *c) {
    (void)c;
    return psys_svc_ready_self() == NEMA_OK ? 0 : NEMA_ERR_NOENT;
}
