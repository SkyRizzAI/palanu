#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libapp.h"
#include "builtin.h"
#include "pal/power.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/settings.h"
#include "pal/sysinfo.h"
#include "pal/time.h"
#include "pal/theme.h"
#include "pal/ui.h"
#include "pal/version.h"

static bool same_name(const char *a, const char *b) { /* ASCII case-insensitive */
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static int cmd_true(int argc, char **argv) { return 0; }
static int cmd_false(int argc, char **argv) { return 1; }

static int cmd_free(int argc, char **argv) {
    char buf[2048];
    size_t n = nema_mem_report(buf, sizeof(buf));
    nema_write(1, buf, n);
    return 0;
}

static const char *state_name(nema_proc_state_t s) { return s == NEMA_PROC_RUNNING ? "run" : s == NEMA_PROC_ZOMBIE ? "zombie" : "-"; }

static int cmd_ps(int argc, char **argv) {
    /* A process record is a few hundred bytes and the table has NEMA_PROC_MAX slots: too big for an app's stack. */
    nema_proc_t *list = nema_alloc(NEMA_MEM_ANY, nema_self() ? nema_self()->owner : NEMA_OWNER_KERNEL, NEMA_PROC_MAX * sizeof(*list));
    if (!list) return 1;
    size_t n = nema_proc_list(list, NEMA_PROC_MAX);
    pal_printf("%4s %4s %-6s %-10s %6s %6s %6s\n", "PID", "PPID", "STATE", "NAME", "HEAP", "STACK", "EXTQ");
    for (size_t i = 0; i < n; i++) {
        nema_owner_stat_t st;
        uint32_t heap = 0, stack = 0, quota = 0;
        if (nema_mem_owner_stat(list[i].owner, &st)) {
            heap = st.used[0] + st.used[1] + st.used[2];
            stack = st.stack;
            quota = st.quota_ext;
        }
        pal_printf("%4d %4d %-6s %-10s %5luK %5luK %5luK\n", list[i].pid, list[i].parent, state_name(list[i].state),
                   list[i].name, (unsigned long)(heap / 1024), (unsigned long)(stack / 1024), (unsigned long)(quota / 1024));
    }
    nema_free(list);
    return 0;
}

static int cmd_top(int argc, char **argv) {
    uint32_t mode = NEMA_TTY_RAW;
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    int cols, rows;
    pal_term_size(&cols, &rows);
    pal_puts("\x1b[?25l\x1b[2J");
    int code = 0;
    pal_cpu_sample_t cpu_s = {0}; /* this top session's own sample state */
    for (;;) {
        char key;
        char line[160];
        pal_puts("\x1b[H");
        int cpus = pal_sys_cpu_count();
        int cpu0 = pal_sys_cpu_percent(&cpu_s, 0), cpu1 = cpus > 1 ? pal_sys_cpu_percent(&cpu_s, 1) : 0;
        uint64_t up = pal_sys_uptime_us() / 1000000u;
        int n = snprintf(line, sizeof(line), "\x1b[7m top  up %02u:%02u:%02u  cpu0 %3d%% cpu1 %3d%% \x1b[0m\x1b[K\r\n",
                         (unsigned)(up / 3600), (unsigned)(up / 60 % 60), (unsigned)(up % 60), cpu0, cpu1);
        nema_write(1, line, (size_t)n);
        n = snprintf(line, sizeof(line), "int %luK/%luK free  ext %luK/%luK free  tasks %lu\x1b[K\r\n",
                     (unsigned long)(pal_sys_mem_free(PAL_MEM_INTERNAL) / 1024),
                     (unsigned long)(pal_sys_mem_total(PAL_MEM_INTERNAL) / 1024),
                     (unsigned long)(pal_sys_mem_free(PAL_MEM_EXTERNAL) / 1024),
                     (unsigned long)(pal_sys_mem_total(PAL_MEM_EXTERNAL) / 1024), (unsigned long)nema_task_count());
        nema_write(1, line, (size_t)n);
        pal_puts("\x1b[1m PID NAME        HEAP  STACK\x1b[0m\x1b[K\r\n");
        nema_proc_t *list = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, NEMA_PROC_MAX * sizeof(*list));
        size_t np = list ? nema_proc_list(list, NEMA_PROC_MAX) : 0;
        int shown = 0;
        for (size_t i = 0; i < np && shown < rows - 4; i++) {
            nema_owner_stat_t st;
            uint32_t heap = 0, stack = 0;
            if (nema_mem_owner_stat(list[i].owner, &st)) {
                heap = st.used[0] + st.used[1] + st.used[2];
                stack = st.stack;
            }
            n = snprintf(line, sizeof(line), "%4d %-10s %4luK %5luK\x1b[K\r\n", list[i].pid, list[i].name,
                         (unsigned long)(heap / 1024), (unsigned long)(stack / 1024));
            nema_write(1, line, (size_t)n);
            shown++;
        }
        nema_free(list);
        pal_puts("\x1b[J");
        int r = nema_read_timeout(0, &key, 1, 1000);
        if (r == NEMA_ERR_INTR) {
            nema_sig_take(NEMA_SIG_FATAL);
            break;
        }
        if (r == 1 && (key == 'q' || key == 3 || key == 27)) break;
        if (r == 0) break;
    }
    pal_puts("\x1b[?25h\x1b[2J\x1b[H");
    return code;
}

static int cmd_log(int argc, char **argv) {
    char *buf = (char *)nema_alloc(NEMA_MEM_ANY, nema_self()->owner, 4096);
    if (!buf) return 1;
    size_t n = nema_log_snapshot(buf, 4096);
    int cols, rows;
    pal_term_size(&cols, &rows);
    int want = argc > 1 ? atoi(argv[1]) : rows - 2;
    size_t start = n;
    int lines = 0;
    while (start > 0 && lines <= want) {
        if (buf[--start] == '\n') lines++;
    }
    if (lines > want) start++;
    for (size_t i = start; i < n; i++) {
        if (buf[i] == '\n') nema_write(1, "\r\n", 2);
        else nema_write(1, &buf[i], 1);
    }
    nema_free(buf);
    return 0;
}

static int cmd_uname(int argc, char **argv) {
    pal_printf("PalanuOS %s %s %s\n", PAL_OS_VERSION, pal_board()->name, pal_board()->chip);
    return 0;
}

static int cmd_uptime(int argc, char **argv) {
    uint64_t up = pal_sys_uptime_us() / 1000000u;
    pal_printf("up %lu days %02u:%02u:%02u\n", (unsigned long)(up / 86400), (unsigned)(up / 3600 % 24), (unsigned)(up / 60 % 60),
               (unsigned)(up % 60));
    return 0;
}

static int cmd_date(int argc, char **argv) {
    if (argc >= 2) { /* date YYYY-MM-DD HH:MM[:SS], interpreted as local time */
        pal_tm_t tm = {0};
        int n = sscanf(argv[1], "%d-%d-%d", &tm.year, &tm.month, &tm.day);
        if (n != 3 || argc < 3) {
            pal_eprintf("usage: date YYYY-MM-DD HH:MM[:SS]\n");
            return 1;
        }
        tm.second = 0;
        if (sscanf(argv[2], "%d:%d:%d", &tm.hour, &tm.minute, &tm.second) < 2) return 1;
        pal_time_set(pal_time_to_epoch(&tm) - (uint32_t)(pal_time_tz_minutes() * 60));
    }
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    pal_tm_t t;
    pal_time_local(&t);
    int tz = pal_time_tz_minutes();
    pal_printf("%s %s %2d %02d:%02d:%02d %c%02d%02d %d\n", days[t.wday], mons[t.month - 1], t.day, t.hour, t.minute, t.second,
               tz < 0 ? '-' : '+', abs(tz) / 60, abs(tz) % 60, t.year);
    return 0;
}

static int cmd_sleep(int argc, char **argv) {
    if (argc < 2) return 1;
    return nema_sleep_ms((int32_t)(atof(argv[1]) * 1000.0)) == NEMA_OK ? 0 : 130;
}

static int cmd_kill(int argc, char **argv) {
    uint32_t sig = NEMA_SIG_TERM;
    int i = 1;
    if (argc > 1 && argv[1][0] == '-') {
        if (strstr(argv[1], "INT")) sig = NEMA_SIG_INT;
        else if (strstr(argv[1], "KILL") || strcmp(argv[1], "-9") == 0) sig = NEMA_SIG_KILL;
        i = 2;
    }
    if (i >= argc) {
        pal_eprintf("usage: kill [-INT|-TERM|-KILL] pid\n");
        return 1;
    }
    nema_err_t r = nema_proc_signal((nema_pid_t)atoi(argv[i]), sig);
    if (r != NEMA_OK) pal_eprintf("kill: %s\n", pal_strerror(r));
    return r == NEMA_OK ? 0 : 1;
}

static int cmd_beep(int argc, char **argv) {
    pal_beep(argc > 1 ? atoi(argv[1]) : 120);
    return 0;
}

static int cmd_battery(int argc, char **argv) {
    int mv = pal_battery_mv(), pct = pal_battery();
    if (mv < 0) {
        pal_printf("no battery detected (or this board cannot measure one)\n");
        return 1;
    }
    pal_printf("%d.%03d V  %d%%%s\n", mv / 1000, mv % 1000, pct, mv > 4300 ? "  (USB power: the percentage is not meaningful)" : "");
    return 0;
}

static int cmd_rgb(int argc, char **argv) {
    if (argc < 4) {
        pal_eprintf("usage: rgb R G B (0-255)\n");
        return 1;
    }
    pal_rgb((uint8_t)atoi(argv[1]), (uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]));
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    pal_puts("rebooting...\n");
    nema_sleep_ms(200);
    pal_ui_splash_restart(); /* the restart splash when the desktop is up, else a plain restart (ui_splash.c) */
    return 0;
}

static void apply_theme(void) {
    pal_display_refresh_theme();
    pal_desktop_invalidate_all();
    pal_settings_save();
}

static int cmd_setting(int argc, char **argv) {
    if (argc != 3) {
        pal_eprintf("usage: setting KEY VALUE\n");
        return 2;
    }
    if (psys_settings_set(argv[1], argv[2]) != NEMA_OK) {
        pal_eprintf("setting: %s not set\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_theme(int argc, char **argv) {
    int idx = pal_theme_index(), size = pal_theme_size();
    bool dark = pal_theme_dark();
    if (argc == 1) {
        pal_printf("theme %s, %s, size %s\nthemes:", pal_theme_name(idx), dark ? "dark" : "light", size ? "medium" : "small");
        for (int i = 0; i < pal_theme_count(); i++) pal_printf(" %s", pal_theme_name(i));
        pal_puts("\nusage: theme NAME | dark | light | small | medium\n");
        return 0;
    }
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "dark") == 0) dark = true;
        else if (strcmp(argv[a], "light") == 0) dark = false;
        else if (strcmp(argv[a], "small") == 0) size = 0;
        else if (strcmp(argv[a], "medium") == 0) size = 1;
        else {
            bool found = false;
            for (int i = 0; i < pal_theme_count(); i++)
                if (same_name(pal_theme_name(i), argv[a])) idx = i, found = true;
            if (!found) {
                pal_eprintf("theme: unknown '%s'\n", argv[a]);
                return 1;
            }
        }
    }
    pal_theme_select(idx, dark, size);
    apply_theme();
    return 0;
}

static int cmd_brightness(int argc, char **argv) {
    if (argc > 1) {
        pal_display_set_brightness(atoi(argv[1]));
        pal_settings_save();
    }
    pal_printf("brightness %d%%\n", pal_display_brightness());
    return 0;
}

static int cmd_flushes(int argc, char **argv) {
    pal_printf("display flushes: %lu\n", (unsigned long)pal_display_flush_count());
    return 0;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
/* lock: the running session's lock screen now (the PIN, when set, unlocks it). */
static int cmd_lock(int argc, char **argv) {
    (void)argc, (void)argv;
    if (pal_desktop_lock() != NEMA_OK) {
        pal_eprintf("lock: no desktop session with a lock screen is running\n");
        return 1;
    }
    return 0;
}

/* power [performance|balanced|saver]: the profile now, or set it (stored, applied at once). */
static int cmd_power(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "performance") && strcmp(argv[1], "balanced") && strcmp(argv[1], "saver")) {
            pal_eprintf("usage: power [performance | balanced | saver]\n");
            return 2;
        }
        if (psys_settings_set("power.profile", argv[1]) != 0) return 1;
    }
    char d[64];
    pal_power_describe(d, sizeof(d));
    pal_printf("%s\n", d);
    return 0;
}

static const nema_app_t SYS_APPS[] = {
    APP("true", "succeed", cmd_true, 0),           APP("false", "fail", cmd_false, 0),
    APP("free", "memory by owner", cmd_free, 0),    APP("ps", "list processes", cmd_ps, 0),
    APP("top", "live system monitor (q quits)", cmd_top, 8192), APP("log", "recent kernel log", cmd_log, 0),
    APP("uname", "system name", cmd_uname, 0),     APP("uptime", "time since boot", cmd_uptime, 0),
    APP("date", "show or set the date", cmd_date, 0), APP("sleep", "wait N seconds", cmd_sleep, 0),
    APP("kill", "signal a process", cmd_kill, 0),  APP("beep", "sound the buzzer", cmd_beep, 0), APP("battery", "supply voltage and charge", cmd_battery, 0),
    APP("rgb", "set the RGB LED", cmd_rgb, 0),     APP("reboot", "restart the board", cmd_reboot, 0),
    APP("lock", "lock the screen now", cmd_lock, 0),
    APP("power", "the power profile: performance, balanced or saver", cmd_power, 0),
    APP("theme", "show or change the theme", cmd_theme, 0), APP("brightness", "backlight percent", cmd_brightness, 0),
    {"flushes", "display flush count", cmd_flushes, 0, NEMA_APP_HIDDEN},
    {"setting", "set a settings key (dev: golden scenes)", cmd_setting, 0, NEMA_APP_HIDDEN},
};

void pal_apps_register_sys(void);
void pal_apps_register_sys(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(SYS_APPS); i++) nema_app_register(&SYS_APPS[i]);
}
