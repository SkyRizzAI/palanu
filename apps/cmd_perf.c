/* Shell commands for the measurement tools: perf, boot, mem. The counters live in system/src/perf.c. boot also shows the
 * boot target and safe mode: `boot target desktop|console` sets the target for the next restart. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "libapp.h"
#include "pal/bootcount.h"
#include "pal/desktop.h"
#include "pal/perf.h"
#include "pal/svc.h"
#include "pal/sysinfo.h"

/* Milliseconds with two decimals from microseconds, without floating point. */
#define MS2(us) (unsigned long)((us) / 1000u), (unsigned long)(((us) % 1000u) / 10u)

static void print_perf(void) {
    pal_perf_stats_t s;
    pal_perf_snapshot(&s);
    pal_printf("%-10s %8s %8s %8s\n", "ms", "last", "mean", "max");
    pal_printf("%-10s %5lu.%02lu %5lu.%02lu %5lu.%02lu\n", "draw", MS2(s.draw_us_last), MS2(s.draw_us_mean),
               MS2(s.draw_us_max));
    pal_printf("%-10s %5lu.%02lu %5lu.%02lu %5lu.%02lu\n", "flush", MS2(s.flush_us_last), MS2(s.flush_us_mean),
               MS2(s.flush_us_max));
    pal_printf("fps          %lu  (1 s: draw %lu.%02lu, flush %lu.%02lu)\n", (unsigned long)s.fps, MS2(s.draw_us_1s),
               MS2(s.flush_us_1s));
    pal_printf("pixels/flush %lu\n", (unsigned long)s.pixels_per_flush);
    pal_printf("flushes      %lu since reset\n", (unsigned long)s.flushes);
    pal_printf("overlay      %s\n", pal_perf_overlay_on() ? "on" : "off");
}

static void print_keys(void) {
    const pal_perf_ring_t *r = pal_perf_key_ring();
    if (r->n == 0) {
        pal_printf("keys: no latencies recorded\n");
        return;
    }
    pal_printf("keys recorded %lu (last %lu kept)\n", (unsigned long)pal_perf_key_count(), (unsigned long)r->n);
    pal_printf("p50 %lu.%02lu ms\n", MS2(pal_perf_ring_percentile(r, 50)));
    pal_printf("p95 %lu.%02lu ms\n", MS2(pal_perf_ring_percentile(r, 95)));
    pal_printf("max %lu.%02lu ms\n", MS2(pal_perf_ring_max(r)));
}

static int cmd_perf(int argc, char **argv) {
    if (argc == 1) {
        print_perf();
        return 0;
    }
    if (strcmp(argv[1], "keys") == 0 && argc == 2) {
        print_keys();
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0 && argc == 2) {
        pal_perf_reset();
        pal_printf("perf counters reset\n");
        return 0;
    }
    if (strcmp(argv[1], "overlay") == 0 && argc == 3 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        bool on = strcmp(argv[2], "on") == 0;
        pal_perf_overlay_set(on);
        if (!on) pal_desktop_invalidate_all(); /* repaint the pixels the overlay covered */
        pal_printf("perf overlay %s\n", on ? "on" : "off");
        return 0;
    }
    pal_eprintf("usage: perf [keys | reset | overlay on|off]\n");
    return 1;
}

static int cmd_boot(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "target") == 0) {
        if (argc < 3 || !pal_settings_set_boot_target(argv[2])) {
            pal_eprintf("usage: boot target [desktop | console]\n");
            return 1;
        }
        pal_printf("boot target: %s (takes effect at the next restart)\n", pal_settings_boot_target());
        return 0;
    }
    if (argc > 1) {
        pal_eprintf("usage: boot [target desktop|console]\n");
        return 1;
    }
    pal_printf("boot target: %s\n", pal_settings_boot_target());
    pal_printf("safe mode: %s\n", psys_svc_safe_mode() ? "yes (`safemode off` clears it at the next boot)" : "no");
    pal_printf("boot attempts: %lu (ready %s)\n", (unsigned long)psys_bootcount_value(), psys_bootcount_is_ready() ? "reached" : "not yet");
    static char buf[512] NEMA_EXT_BSS;
    size_t n = pal_boot_report(buf, sizeof(buf));
    nema_write(1, buf, n);
    return 0;
}

static int cmd_heapcheck(int argc, char **argv) {
    bool ok = pal_sys_heap_check();
    pal_puts(ok ? "heap: intact\n" : "heap: DAMAGED (the block is printed on the console)\n");
    return ok ? 0 : 1;
}

static int cmd_mem(int argc, char **argv) {
    static char buf[3072] NEMA_EXT_BSS;
    size_t n = nema_mem_report_ex(buf, sizeof(buf));
    nema_write(1, buf, n);
    return 0;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t PERF_APPS[] = {
    APP("perf", "frame and key latency counters (keys: since perf reset)", cmd_perf, 0),
    APP("boot", "boot: target, safe mode, stage times", cmd_boot, 0),
    APP("mem", "memory by class and owner", cmd_mem, 0),
    APP("heapcheck", "verify the heap block headers", cmd_heapcheck, 0),
};

void pal_apps_register_perf(void);
void pal_apps_register_perf(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(PERF_APPS); i++) nema_app_register(&PERF_APPS[i]);
}
