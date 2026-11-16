/* PalanuOS wall clock. There is no battery-backed RTC on hhv2, so time starts at the firmware build time
 * and is corrected by the user, by SNTP once Wi-Fi is up, or by a connected Palanu Link client. */
#ifndef PAL_TIME_H
#define PAL_TIME_H

#include <stdint.h>

typedef struct {
    int year, month, day, hour, minute, second, wday; /* month 1..12, wday 0 = Sunday */
} pal_tm_t;

void pal_time_init(void);
uint32_t pal_time_epoch(void);                 /* seconds since 1970-01-01 UTC */
void pal_time_set(uint32_t epoch);
void pal_time_set_tz_minutes(int minutes);     /* offset from UTC */
int pal_time_tz_minutes(void);
void pal_time_local(pal_tm_t *out);            /* local time = epoch + tz */
void pal_time_from_epoch(uint32_t epoch, pal_tm_t *out);
uint32_t pal_time_to_epoch(const pal_tm_t *tm);

#endif
