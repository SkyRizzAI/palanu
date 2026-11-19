/* Tones and tunes on the buzzer (the board's PAL_DEV_BUZZER). A tune is RTTTL text, the ringtone format of old phones:
 * "name:d=4,o=5,b=120:8c,8e,g,2c6" (default duration, octave and tempo, then notes: [length]note[#][.][octave][.], p a rest).
 * Playing blocks the caller for the tune's length and stops early when the process is interrupted (Ctrl-C). */
#ifndef PAL_MUSIC_H
#define PAL_MUSIC_H

#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"

typedef struct {
    uint16_t freq; /* Hz; 0 is a rest */
    uint16_t ms;
} pal_note_t;

/* The notes of an RTTTL tune, at most max; name (may be NULL) gets the tune's name. -1 when it is not RTTTL. */
int pal_rtttl_parse(const char *text, pal_note_t *out, size_t max, char *name, size_t namecap);
/* One tone (freq 0: silence) for ms, then returns. */
void pal_tone(int freq_hz, int ms);
/* Plays an RTTTL tune: NEMA_OK, NEMA_ERR_INVAL (not RTTTL), NEMA_ERR_INTR (interrupted), NEMA_ERR_NOENT (no buzzer). */
nema_err_t pal_music_play(const char *rtttl);
/* The tunes the system carries (by name: scale, twinkle, ode, alert), or NULL. */
const char *pal_music_builtin(const char *name);
const char *pal_music_builtin_at(size_t i); /* the i-th name, NULL past the last */

#endif
