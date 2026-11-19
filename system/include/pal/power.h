/* PalanuOS power profiles: performance, balanced and saver (the power.profile setting). The system applies the stored profile at
 * boot and on every change; the board turns it into a CPU frequency policy (dynamic frequency scaling on the ESP32). A board
 * without the operations keeps its clock and reports so. Saver also lets the chip light-sleep between tasks while the screen is
 * off (pal_power_screen), on a board that offers light_sleep: the timers that wake the tasks (the keyboard's poll, the radio's
 * beacons) wake it too. hhv2 does not offer it yet (platform/esp-idf/palanu/power_esp.c, sdkconfig.defaults). */
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
    /* Appended later; may be NULL. Allow the chip to light-sleep whenever every task waits (automatic light sleep). */
    nema_err_t (*light_sleep)(bool allow);
} pal_power_ops_t;

void pal_power_set_ops(const pal_power_ops_t *ops);
/* Applies a profile now (it is not stored: power.profile is, and changing it calls this). */
nema_err_t pal_power_apply(int profile);
int pal_power_profile(void); /* the profile applied last */
void pal_power_describe(char *out, size_t cap); /* "Balanced: CPU 80 to 320 MHz", or why it is not applied */
/* The screen went on or off (the display power manager calls it): Saver light-sleeps while it is off. */
void pal_power_screen(bool on);
bool pal_power_sleeping(void); /* light sleep is allowed now */

#endif
