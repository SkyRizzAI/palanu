/* Services (API level 2). A service is a background app the supervisor starts (kind=service); it calls pal_svc_ready when
 * it has finished starting, which makes its state RUNNING at once instead of after the start timeout. No permission is
 * needed for this call. Starting, stopping, and reading other services is the svc shell command and, later, the svc.control
 * permission. */
#ifndef PALANU_API_SVC_H
#define PALANU_API_SVC_H

#include "annot.h"

PAL_SINCE(2) PAL_WASM(svc_ready)
PAL_DOC("Tells the supervisor that this service has finished starting. Returns 0, or PAL_ERR_NOENT when the caller is not a service the supervisor started.")
int32_t pal_svc_ready(void);

#endif
