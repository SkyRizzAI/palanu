/* Service commands: svc (list, status, start, stop, restart, enable, disable, logs) and safemode. The boot
 * command is in cmd_perf.c, next to the boot stage report.
 * The supervisor does the work (system/src/svc.c); these commands show it and send it requests. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/bootcount.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/sysinfo.h"

#define LOG_CHUNK 256u

static void uptime_text(uint64_t us, char *out, size_t cap) {
    uint64_t s = us / 1000000u;
    snprintf(out, cap, "%02u:%02u:%02u", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
}

static void safe_banner(void) {
    if (!psys_svc_safe_mode()) return;
    pal_printf("*** SAFE MODE: the last %u boots did not reach ready. Only core services run; installed apps are not scanned.\n",
               (unsigned)PSYS_SAFE_BOOT_TRIES);
    pal_printf("    Fix the cause, then `safemode off` (takes effect at the next boot).\n");
}

static int cmd_list(void) {
    safe_banner();
    size_t n = psys_svc_count();
    pal_printf("%-14s %-15s %-6s %-9s %-9s %-4s %s\n", "ID", "STATE", "PID", "RESTARTS", "UPTIME", "AUTO", "SOURCE");
    for (size_t i = 0; i < n; i++) {
        psys_svc_info_t in;
        if (!psys_svc_info(i, &in)) continue;
        char pid[8] = "-", up[16] = "-";
        if (in.pid) snprintf(pid, sizeof(pid), "%d", (int)in.pid);
        if (in.state == PSYS_SVC_RUNNING && in.started_us) uptime_text(pal_sys_uptime_us() - in.started_us, up, sizeof(up));
        const char *src = in.installed ? "installed" : (in.core ? "built-in, core" : "built-in");
        pal_printf("%-14s %-15s %-6s %-9lu %-9s %-4s %s\n", in.id, psys_svc_state_name(in.state), pid,
                   (unsigned long)in.restarts, up, in.enabled ? "on" : "off", src);
        if (in.reason[0] && in.state != PSYS_SVC_RUNNING) pal_printf("%14s %s\n", "", in.reason);
    }
    if (!n) pal_printf("no services\n");
    size_t sp = psys_svc_stack_spare();
    if (sp != (size_t)-1) pal_printf("supervisor stack: at least %lu bytes spare\n", (unsigned long)sp);
    return 0;
}

static int cmd_status(const char *id) {
    psys_svc_info_t in;
    if (!psys_svc_find(id, &in)) {
        pal_eprintf("svc: no service '%s'\n", id);
        return 1;
    }
    char up[16] = "-";
    if (in.state == PSYS_SVC_RUNNING && in.started_us) uptime_text(pal_sys_uptime_us() - in.started_us, up, sizeof(up));
    pal_printf("%s (%s)\n", in.id, in.name);
    pal_printf("  state     %s%s%s\n", psys_svc_state_name(in.state), in.reason[0] ? ": " : "", in.reason);
    pal_printf("  enabled   %s   (starts at boot)\n", in.enabled ? "yes" : "no");
    pal_printf("  source    %s%s\n", in.installed ? "installed package" : "built-in", in.core ? ", core (cannot be stopped)" : "");
    char pid[8] = "-";
    if (in.pid) snprintf(pid, sizeof(pid), "%d", (int)in.pid);
    pal_printf("  pid       %s\n", pid);
    pal_printf("  restarts  %lu, failures in the last 10 minutes: %lu\n", (unsigned long)in.restarts, (unsigned long)in.failures);
    pal_printf("  uptime    %s\n", up);
    pal_printf("  log       %lu bytes written (`svc logs %s`)\n", (unsigned long)in.log_total, in.id);
    return 0;
}

static int report(nema_err_t r, const char *id) {
    if (r == NEMA_OK) return 0;
    if (r == NEMA_ERR_NOENT) pal_eprintf("svc: no service '%s'\n", id);
    else if (r == NEMA_ERR_PERM) pal_eprintf("svc: %s is core and cannot be stopped\n", id);
    else if (r == NEMA_ERR_IO) {
        psys_svc_info_t in;
        pal_eprintf("svc: %s failed%s%s\n", id, psys_svc_find(id, &in) && in.reason[0] ? ": " : "", in.reason);
    } else if (r == NEMA_ERR_INTR) pal_eprintf("svc: interrupted\n");
    else pal_eprintf("svc: the supervisor did not answer (%s)\n", pal_strerror(r));
    return 1;
}

/* After a start or restart: a service that did not come up says why (waiting for memory, for a condition, or failed). */
static int report_started(nema_err_t r, const char *id) {
    if (report(r, id) != 0) return 1;
    psys_svc_info_t in;
    if (psys_svc_find(id, &in) && in.state != PSYS_SVC_RUNNING)
        pal_printf("svc: %s is %s%s%s\n", id, psys_svc_state_name(in.state), in.reason[0] ? ": " : "", in.reason);
    return 0;
}

/* svc logs ID [-f]: what is kept, then (with -f) every new line until Ctrl-C. */
static int cmd_logs(const char *id, bool follow) {
    psys_svc_info_t in;
    if (!psys_svc_find(id, &in)) {
        pal_eprintf("svc: no service '%s'\n", id);
        return 1;
    }
    uint64_t cursor = 0;
    char buf[LOG_CHUNK];
    for (;;) {
        int n = psys_svc_log_read(id, &cursor, buf, sizeof(buf));
        if (n < 0) return 1;
        if (n > 0) {
            nema_write(1, buf, (size_t)n);
            continue;
        }
        if (!follow) return 0;
        nema_err_t w = psys_svc_log_wait(id, cursor, 1000);
        if (w == NEMA_ERR_INTR) return 0; /* Ctrl-C */
    }
}

static int cmd_svc(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "list") == 0) return cmd_list();
    const char *op = argv[1];
    if (strcmp(op, "logs") == 0) {
        if (argc < 3) goto usage;
        bool follow = argc > 3 && strcmp(argv[3], "-f") == 0;
        return cmd_logs(argv[2], follow);
    }
    if (argc < 3) goto usage;
    const char *id = argv[2];
    if (strcmp(op, "status") == 0) return cmd_status(id);
    if (strcmp(op, "start") == 0) return report_started(psys_svc_start(id), id);
    if (strcmp(op, "stop") == 0) return report(psys_svc_stop(id), id);
    if (strcmp(op, "restart") == 0) return report_started(psys_svc_restart(id), id);
    if (strcmp(op, "enable") == 0) return report(psys_svc_set_enabled(id, true), id);
    if (strcmp(op, "disable") == 0) return report(psys_svc_set_enabled(id, false), id);
usage:
    pal_eprintf("usage: svc [list | status ID | start ID | stop ID | restart ID | enable ID | disable ID | logs ID [-f]]\n");
    return 1;
}

static int cmd_safemode(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "off") == 0) {
        if (psys_bootcount_clear() != NEMA_OK) {
            pal_eprintf("safemode: the boot counter cannot be cleared on this board\n");
            return 1;
        }
        pal_printf("safemode: the boot counter is cleared; normal boot at the next restart\n");
        return 0;
    }
    if (argc > 1) {
        pal_eprintf("usage: safemode [off]\n");
        return 1;
    }
    pal_printf("safe mode: %s (boot attempts %lu of %u before safe mode; ready %s)\n",
               psys_svc_safe_mode() ? "on" : "off", (unsigned long)psys_bootcount_value(), (unsigned)PSYS_SAFE_BOOT_TRIES,
               psys_bootcount_is_ready() ? "reached" : "not yet");
    return 0;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t SVC_APPS[] = {
    APP("svc", "services: list, status, start, stop, enable, logs", cmd_svc, 8192),
    APP("safemode", "safe mode: status, off", cmd_safemode, 4096),
};

void pal_apps_register_svc(void);
void pal_apps_register_svc(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(SVC_APPS); i++) nema_app_register(&SVC_APPS[i]);
}
