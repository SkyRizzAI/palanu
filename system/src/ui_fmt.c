/* Text for the screens: clock, date and the age of a notification. Pure functions, tested on the host. */
#include <stdbool.h>
#include <stdio.h>

#include "pal/time.h"
#include "pal/ui.h"

static const char *const WDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static bool s_clock24 = true; /* ui.clock24, set by the setting (settings_kv.c) */

void pal_ui_set_clock24(bool on) { s_clock24 = on; }
bool pal_ui_clock24(void) { return s_clock24; }

int pal_ui_hour(int hour) {
    if (s_clock24) return hour;
    int h = hour % 12;
    return h ? h : 12;
}

const char *pal_ui_ampm(int hour) { return s_clock24 ? "" : hour % 24 < 12 ? "AM" : "PM"; }

void pal_ui_fmt_hm(int hour, int minute, char *out, size_t cap) {
    if (s_clock24) snprintf(out, cap, "%02d:%02d", hour % 100, minute % 100);
    else snprintf(out, cap, "%d:%02d", pal_ui_hour(hour % 24), minute % 100);
}

void pal_ui_fmt_clock(int hour, int minute, char *out, size_t cap) {
    char hm[8];
    pal_ui_fmt_hm(hour, minute, hm, sizeof(hm));
    snprintf(out, cap, "%s%s%s", hm, s_clock24 ? "" : " ", pal_ui_ampm(hour));
}

void pal_ui_fmt_date(const pal_tm_t *tm, char *out, size_t cap) {
    snprintf(out, cap, "%s %d %s %d", WDAY[tm->wday % 7], tm->day, MON[(tm->month + 11) % 12], tm->year);
}

void pal_ui_fmt_age(uint32_t now, uint32_t then, int tz_minutes, char *out, size_t cap) {
    uint32_t d = now >= then ? now - then : 0;
    if (d < 60) {
        snprintf(out, cap, "now");
        return;
    }
    if (d < 3600) {
        snprintf(out, cap, "%um", (unsigned)(d / 60));
        return;
    }
    pal_tm_t a, b;
    pal_time_from_epoch(now + (uint32_t)(int32_t)(tz_minutes * 60), &a);
    pal_time_from_epoch(then + (uint32_t)(int32_t)(tz_minutes * 60), &b);
    if (a.year == b.year && a.month == b.month && a.day == b.day) {
        pal_ui_fmt_clock(b.hour, b.minute, out, cap);
        return;
    }
    unsigned days = d / 86400u;
    snprintf(out, cap, "%ud", days ? days : 1u);
}
