/* PalanuOS power profiles: performance, balanced and saver (the power.profile setting). The system applies the stored profile at
 * boot and on every change; the board turns it into a CPU frequency policy (dynamic frequency scaling on the ESP32). A board
 * without the operations keeps its clock and reports so. Light sleep is not used yet: it needs every wake source (the keyboard's
 * interrupt, the UART) checked on the board first. */
#ifndef PAL_POWER_H
#define PAL_POWER_H

#include <stdbool.h>
#include <stddef.h>

#include "nema/types.h"

enum { PAL_POWER_PERFORMANCE = 0, PAL_POWER_BALANCED = 1, PAL_POWER_SAVER = 2 };

typedef struct {
    /* the CPU between min_mhz and max_mhz (equal: a fixed clock); NEMA_ERR_UNSUPPORTED when the board cannot */
    nema_err_t (*cpu)(int min_mhz, int max_mhz);
    int max_mhz; /* the highest clock the board runs */
} pal_power_ops_t;

void pal_power_set_ops(const pal_power_ops_t *ops);
/* Applies a profile now (it is not stored: power.profile is, and changing it calls this). */
nema_err_t pal_power_apply(int profile);
int pal_power_profile(void); /* the profile applied last */
void pal_power_describe(char *out, size_t cap); /* "Balanced: CPU 80 to 320 MHz", or why it is not applied */

#endif
