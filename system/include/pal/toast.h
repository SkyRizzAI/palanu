/* PalanuOS toasts: a short message in a bar above everything, removed by a timer. Posted through the system toast
 * queue (pal/notify.h), so any task may post one. The queue holds 4; the desktop shows one at a time. */
#ifndef PAL_TOAST_H
#define PAL_TOAST_H

#include <stdbool.h>
#include <stdint.h>

#include "nema/types.h"

#define PAL_TOAST_MS_DEFAULT 2000
#define PAL_TOAST_CHARS 40 /* longer text is cut to this many characters on the screen */

/* Post a toast (source "system"). ms <= 0 means PAL_TOAST_MS_DEFAULT. Returns NEMA_OK or the notify error (BUSY, FULL). */
nema_err_t psys_toast(const char *text, int ms);

/* Desktop side (system/src/toast.c). The desktop calls service on every loop with its clock: it expires the shown toast and
 * takes the next one from the queue. Returns true when the shown toast changed (the desktop then redraws in full). */
bool pal_toast_service(uint32_t now_ms);
/* Milliseconds until the shown toast ends, or -1 when none is shown. */
int32_t pal_toast_next_ms(uint32_t now_ms);
bool pal_toast_showing(void);
const char *pal_toast_text(void); /* the shown text, "" when none */
void pal_toast_reset(void);       /* tests: forget the shown toast */

#endif
