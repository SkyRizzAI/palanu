/* Firmware update: the platform supplies how to write the inactive image; the link protocol drives it. */
#ifndef PAL_OTA_H
#define PAL_OTA_H

#include "nema/nema.h"

typedef struct {
    nema_err_t (*begin)(size_t size);              /* choose the inactive slot and prepare it; size is the image length */
    nema_err_t (*write)(const void *data, size_t n);
    nema_err_t (*end)(void);                       /* verify the image and make it the next boot */
    void (*abort)(void);
    const char *(*running)(void);                  /* label of the running slot, e.g. "ota_0"; may be NULL */
    const char *(*version)(void);                  /* build identifier of the running image; may be NULL */
} pal_ota_ops_t;

void pal_ota_set(const pal_ota_ops_t *ops);
const pal_ota_ops_t *pal_ota(void); /* NULL on a platform without update support (the simulator) */

#endif
