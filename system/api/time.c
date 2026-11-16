/* The time module of the palanu API (api/palanu/time.h): the clock since boot, sleeping, and the calendar time (UTC
 * seconds, and the local time in the system timezone). */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"
#include "nema/port.h"
#include "pal/time.h"

int64_t psys_api_now_ms(api_ctx_t *c) {
    (void)c;
    return (int64_t)(nema_port_time_us() / 1000u);
}

/* Sleeps in slices of 100 ms, so an interrupt or kill is noticed within one slice. */
void psys_api_sleep_ms(api_ctx_t *c, int32_t ms) {
    while (ms > 0) {
        int32_t slice = ms > 100 ? 100 : ms;
        nema_sleep_ms(slice);
        ms -= slice;
        if (nema_interrupted() || nema_canceled()) {
            api_stop(c, "interrupted");
            return;
        }
    }
}

int64_t psys_api_unix_time(api_ctx_t *c) {
    (void)c;
    return (int64_t)pal_time_epoch();
}

/* The layout of C's struct tm, as the API documents it: sec, min, hour, mday, mon (0-11), year (since 1900), wday, yday,
 * isdst (always 0). The timezone is the system's (pal_time_set_tz_minutes). */
int32_t psys_api_localtime(api_ctx_t *c, int64_t unix_s, void *tm_out, uint32_t len) {
    (void)c;
    if (len < 36) return NEMA_ERR_INVAL;
    int64_t local = unix_s + (int64_t)pal_time_tz_minutes() * 60;
    if (local < 0 || local > (int64_t)UINT32_MAX) return NEMA_ERR_RANGE;
    pal_tm_t t;
    pal_time_from_epoch((uint32_t)local, &t);
    pal_tm_t jan1 = {.year = t.year, .month = 1, .day = 1};
    int32_t yday = (int32_t)(local / 86400 - (int64_t)(pal_time_to_epoch(&jan1) / 86400));
    int32_t f[9] = {t.second, t.minute, t.hour, t.day, t.month - 1, t.year - 1900, t.wday, yday, 0};
    for (int i = 0; i < 9; i++) api_put32((uint8_t *)tm_out + 4 * i, (uint32_t)f[i]);
    return NEMA_OK;
}
