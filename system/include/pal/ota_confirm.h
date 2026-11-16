/* OTA confirmation with a health check. A new image is confirmed only when the system has proved it works:
 * the desktop drew a frame, the UART link is running (service link-uart), and no radio failed. The check has a window of
 * 60 seconds. If the window ends first, the image is NOT confirmed and the board reboots, so the bootloader rolls back to
 * the previous image (ESP-IDF rollback: an image still pending verification at boot is abandoned). The reboot happens only
 * when the running image is pending verification; a board flashed over USB is never rebooted by this check. */
#ifndef PAL_OTA_CONFIRM_H
#define PAL_OTA_CONFIRM_H

#include <stdbool.h>
#include <stdint.h>

#include "nema/nema.h"

#define PSYS_OTA_WINDOW_MS 60000u
#define PSYS_OTA_PERIOD_MS 250u

typedef struct {
    bool (*pending)(void); /* the running image is awaiting its first confirmation */
    bool (*confirm)(void); /* marks the running image valid; true on success */
    void (*reboot)(void);  /* restarts the board (the bootloader then rolls back an unconfirmed image) */
} pal_ota_confirm_ops_t;

typedef struct {
    bool desktop_frame; /* the desktop has drawn at least one frame */
    bool link_up;       /* service link-uart is RUNNING */
    bool radios_ok;     /* no radio is FAILED */
} psys_ota_health_t;

typedef enum {
    PSYS_OTA_WAIT = 0,
    PSYS_OTA_CONFIRM,
    PSYS_OTA_ROLLBACK,
} psys_ota_action_t;

/* The policy alone. elapsed_ms counts from the start of the check. why (optional) receives a text for a rollback. */
psys_ota_action_t psys_ota_decide(const psys_ota_health_t *h, uint32_t elapsed_ms, char *why, size_t cap);
/* The health now, read from the desktop, the radios, and the service table. */
psys_ota_health_t psys_ota_health_read(void);

void psys_ota_confirm_set_ops(const pal_ota_confirm_ops_t *ops); /* NULL on a board without updates */
/* The service's start: starts the check in its own task. NEMA_OK at once; a second call does nothing. */
nema_err_t psys_ota_confirm_start(void);

#endif
