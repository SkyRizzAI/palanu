/* Time: the clock since boot (for measuring), sleeping, and the calendar time (for showing). Sleeping and the wait calls
 * stop an app that Ctrl-C or kill has asked to end. The calendar clock has no battery: it starts at the firmware's build
 * time until the user, SNTP or a connected Palanu Link client sets it. */
#ifndef PALANU_API_TIME_H
#define PALANU_API_TIME_H

#include "annot.h"

PAL_SINCE(1) PAL_WASM(time_ms) PAL_ALIAS(pal_time_ms)
PAL_DOC("Milliseconds since boot.")
int64_t pal_now_ms(void);

PAL_SINCE(1) PAL_WASM(sleep_ms)
PAL_DOC("Sleeps for ms milliseconds, or until the app is interrupted.")
void pal_sleep_ms(int32_t ms);

PAL_SINCE(2) PAL_WASM(unix_time)
PAL_DOC("Seconds since 1970-01-01 00:00 UTC, from the calendar clock.")
int64_t pal_unix_time(void);

PAL_SINCE(2) PAL_WASM(localtime)
PAL_DOC("Writes the local time of unix_s (seconds since 1970 UTC) into tm_out, 36 bytes (len must be at least 36): nine little-endian int32 sec, min, hour, mday, mon (0 to 11), year (years since 1900), wday (0 is Sunday), yday (0 to 365), isdst (always 0). The local time is the system timezone. Returns 0, or a negative error (outside 1970 to 2106, or the buffer is too small).")
int32_t pal_localtime(int64_t unix_s, PAL_OUT(len) void *tm_out, uint32_t len);

#endif
