/* The notify shell command: lists, posts and clears the notifications apps post. It stands in for the notification
 * center, which is not drawn yet. Every post is also logged by the system (system/src/notify.c). */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "libapp.h"
#include "pal/notify.h"
#include "pal/time.h"

/* Calendar text of a time in seconds since 1970 UTC, in the system's time zone. */
static void stamp(uint32_t epoch, char *out, size_t cap) {
    pal_tm_t t;
    pal_time_from_epoch((uint32_t)((int64_t)epoch + (int64_t)pal_time_tz_minutes() * 60), &t);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
}

static void print_list(void) {
    static psys_notify_t list[PSYS_NOTIFY_RING] NEMA_EXT_BSS;
    int n = psys_notify_list(list, PSYS_NOTIFY_RING);
    if (n == 0) {
        pal_printf("no notifications\n");
        return;
    }
    pal_printf("   %-5s %-10s %-16s %s\n", "id", "from", "time", "notification");
    for (int i = 0; i < n; i++) {
        char when[24];
        stamp(list[i].time, when, sizeof(when));
        pal_printf("%s  %-5lu %-10s %-16s %s: %s\n", list[i].read ? " " : "*", (unsigned long)list[i].id, list[i].source,
                   when, list[i].title, list[i].text);
    }
    psys_notify_mark_all_read(); /* the marks show what is new since the last list */
}

static int cmd_notify(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        print_list();
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "post") == 0) {
        int32_t id = psys_notify_post("system", argv[2], argv[3], 0);
        if (id < 0) {
            pal_eprintf("notify: %s\n", pal_strerror(id));
            return 1;
        }
        pal_printf("posted %ld\n", (long)id);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        psys_notify_clear();
        pal_printf("notifications cleared\n");
        return 0;
    }
    pal_eprintf("usage: notify [list | post TITLE TEXT | clear]\n");
    return 1;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t NOTIFY_APPS[] = {
    APP("notify", "notifications posted by apps (list, post TITLE TEXT, clear)", cmd_notify, 0),
};

void pal_apps_register_notify(void);
void pal_apps_register_notify(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(NOTIFY_APPS); i++) nema_app_register(&NOTIFY_APPS[i]);
}
