/* PalanuOS display power manager: sleep the screen after idle, lock after a further idle time.
 *
 * States: ACTIVE (display on, unlocked); SLEEP (backlight off, unlocked; the first key wakes the screen and is consumed);
 * LOCKED (the lock view is shown; the display may be on or off). The desktop owns the lock view; the timers here are pure
 * functions of a millisecond clock passed in, so tests run with a fake clock. Settings: dpm.sleep_ms (default 60000, 0 =
 * never) and dpm.lock_ms (default 0 = never; counted from the moment the display sleeps). The PIN is stored as a salted
 * SHA-256 hash under security.pin in /data/settings. There is no DIM state yet.
 */
#ifndef PAL_DPM_H
#define PAL_DPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"

#define PAL_DPM_SLEEP_DEFAULT_MS 60000u
#define PAL_DPM_LOCK_DEFAULT_MS 0u

typedef enum {
    PAL_DPM_ACTIVE = 0,
    PAL_DPM_SLEEP,
    PAL_DPM_LOCKED,
} pal_dpm_state_t;

typedef struct {
    void (*display)(bool on);  /* backlight; may be NULL */
    bool (*lock)(void);        /* the lock timeout fired: show the lock screen; true when it is shown */
} pal_dpm_hooks_t;

void pal_dpm_init(const pal_dpm_hooks_t *hooks, uint32_t now_ms);
void pal_dpm_set_sleep_ms(uint32_t ms);
void pal_dpm_set_lock_ms(uint32_t ms);
uint32_t pal_dpm_sleep_ms(void);
uint32_t pal_dpm_lock_ms(void);

/* A key press. Returns true when the key only woke the display: the caller must not deliver it. Resets the idle timer. */
bool pal_dpm_key(uint32_t now_ms);
/* Activity that is not a key (Link input): resets the idle timer, never wakes the display. */
void pal_dpm_activity(uint32_t now_ms);
/* Run the timers. Call on every loop. */
void pal_dpm_tick(uint32_t now_ms);
/* Milliseconds until the next timer fires, or -1 when none is armed. */
int32_t pal_dpm_next_ms(uint32_t now_ms);

pal_dpm_state_t pal_dpm_state(void);
bool pal_dpm_display_on(void);
bool pal_dpm_locked(void);
/* The desktop sets this when it shows (true) or removes (false) the lock view. */
void pal_dpm_set_locked(bool locked, uint32_t now_ms);

/* PIN. pal_dpm_pin_set with "" clears it; a PIN is 4 to 16 characters, printable ASCII. Returns NEMA_OK or NEMA_ERR_INVAL. */
nema_err_t pal_dpm_pin_set(const char *pin);
bool pal_dpm_pin_is_set(void);
bool pal_dpm_pin_check(const char *pin);
/* Text form for the settings file ("salt:hash" in hex, "" when unset), and the reverse at load. */
void pal_dpm_pin_text(char *out, size_t cap);
void pal_dpm_pin_load(const char *text);

#endif
