/* Boot-attempt counter and safe mode. The counter lives where the platform keeps small settings that survive a
 * reset (NVS on the board, a file in the data directory on the host). It goes up at the start of every boot. The system
 * clears it 30 seconds after a boot reaches "ready": every autostart service is running, failed, or waiting for memory.
 * A boot that starts with the counter at SAFE_BOOT_TRIES (3) runs in safe mode: no autostart except core services, no
 * scan of installed apps, and the Terminal in front. `safemode off` clears the counter; safe mode then ends at the next
 * boot. */
#ifndef PAL_BOOTCOUNT_H
#define PAL_BOOTCOUNT_H

#include <stdbool.h>
#include <stdint.h>

#include "nema/nema.h"

#define PSYS_SAFE_BOOT_TRIES 3u
#define PSYS_BOOT_READY_CLEAR_MS (30u * 1000u)

typedef struct {
    uint32_t (*get)(void);         /* the stored count; 0 when nothing is stored */
    nema_err_t (*set)(uint32_t n); /* stores the count; NEMA_OK or an error */
} pal_bootcount_ops_t;

/* Called once at the start of boot, with the platform's ops (NULL: no counter, never safe mode). Increments the count
 * and returns true when this boot must run in safe mode. */
bool psys_bootcount_begin(const pal_bootcount_ops_t *ops);
bool psys_bootcount_safe(void);
uint32_t psys_bootcount_value(void); /* the count this boot started with (after its increment) */

/* Called by the supervisor on every pass with whether boot is settled. Clears the counter once the boot has been ready for
 * PSYS_BOOT_READY_CLEAR_MS, unless this boot is in safe mode. */
void psys_bootcount_ready_check(uint64_t now_us, bool settled);
bool psys_bootcount_is_ready(void);

/* `safemode off`: clears the counter. Safe mode ends at the next boot. */
nema_err_t psys_bootcount_clear(void);

/* Test seams: the default clock is the port's. */
void psys_bootcount_reset_for_test(void);

#endif
