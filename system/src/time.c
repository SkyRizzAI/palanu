#include <string.h>

#include "nema/port.h"
#include "pal/time.h"

static struct {
    int64_t offset_s; /* epoch = offset + uptime seconds */
    int tz_min;
} C;

static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

uint32_t pal_time_to_epoch(const pal_tm_t *tm) {
    int64_t days = 0;
    for (int y = 1970; y < tm->year; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < tm->month; m++) days += DAYS[m - 1] + (m == 2 && leap(tm->year) ? 1 : 0);
    days += tm->day - 1;
    return (uint32_t)(days * 86400 + tm->hour * 3600 + tm->minute * 60 + tm->second);
}

void pal_time_from_epoch(uint32_t epoch, pal_tm_t *o) {
    uint32_t days = epoch / 86400, rem = epoch % 86400;
    o->hour = (int)(rem / 3600);
    o->minute = (int)(rem % 3600 / 60);
    o->second = (int)(rem % 60);
    o->wday = (int)((days + 4) % 7);
    int y = 1970;
    for (;;) {
        uint32_t n = leap(y) ? 366 : 365;
        if (days < n) break;
        days -= n;
        y++;
    }
    o->year = y;
    int m = 0;
    for (;; m++) {
        uint32_t n = (uint32_t)(DAYS[m] + (m == 1 && leap(y) ? 1 : 0));
        if (days < n) break;
        days -= n;
    }
    o->month = m + 1;
    o->day = (int)days + 1;
}

/* __DATE__ is "Mmm dd yyyy", __TIME__ is "hh:mm:ss". */
static uint32_t build_epoch(void) {
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    pal_tm_t tm = {0};
    for (int i = 0; i < 12; i++)
        if (strncmp(d, mon + i * 3, 3) == 0) tm.month = i + 1;
    tm.day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
    tm.year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    const char *t = __TIME__;
    tm.hour = (t[0] - '0') * 10 + (t[1] - '0');
    tm.minute = (t[3] - '0') * 10 + (t[4] - '0');
    tm.second = (t[6] - '0') * 10 + (t[7] - '0');
    return pal_time_to_epoch(&tm);
}

void pal_time_init(void) {
    C.tz_min = 7 * 60; /* the board's owner is on WIB; changeable in settings */
    /* __DATE__/__TIME__ are the builder's local time, so convert to UTC before using them as an epoch */
    C.offset_s = (int64_t)build_epoch() - (int64_t)C.tz_min * 60 - (int64_t)(nema_port_time_us() / 1000000u);
}

uint32_t pal_time_epoch(void) { return (uint32_t)(C.offset_s + (int64_t)(nema_port_time_us() / 1000000u)); }
void pal_time_set(uint32_t e) { C.offset_s = (int64_t)e - (int64_t)(nema_port_time_us() / 1000000u); }
void pal_time_set_tz_minutes(int m) { C.tz_min = m; }
int pal_time_tz_minutes(void) { return C.tz_min; }
void pal_time_local(pal_tm_t *o) { pal_time_from_epoch((uint32_t)((int64_t)pal_time_epoch() + C.tz_min * 60), o); }
