/* Nema registry: subsystems publish themselves by name; users wait until the name appears.
 * This replaces hand-ordered start-up: a consumer that needs "display" just asks for it. */
#ifndef NEMA_REGISTRY_H
#define NEMA_REGISTRY_H

#include "nema/types.h"

nema_err_t nema_registry_publish(const char *name, void *ptr); /* NEMA_ERR_EXIST if already published */
nema_err_t nema_registry_unpublish(const char *name);
/* Returns NEMA_OK with *out set, NEMA_ERR_NOENT after the timeout, NEMA_ERR_INTR if interrupted. */
nema_err_t nema_registry_get(const char *name, void **out, int32_t timeout_ms);

#endif
