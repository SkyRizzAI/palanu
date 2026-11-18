/* Monitor: processor and memory use, as a form (ADR-0010 §8). The rows are data (read-only gauges and values) bound to the view, so
 * the running desktop session lays them out in its own look: the classic theme's list, or Athena's grouped list. Sampled twice a
 * second while it is in front. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/menu.h"
#include "pal/sysinfo.h"
#include "pal/theme.h"

static int cpu[2];
static size_t mem_t[2], mem_f[2]; /* internal, external */
static size_t procs;
static pal_cpu_sample_t cpu_s; /* Monitor's own sample state; top keeps its own */

static void sample(void) {
    cpu[0] = pal_sys_cpu_percent(&cpu_s, 0);
    cpu[1] = pal_sys_cpu_count() > 1 ? pal_sys_cpu_percent(&cpu_s, 1) : 0;
    mem_t[0] = pal_sys_mem_total(PAL_MEM_INTERNAL), mem_f[0] = pal_sys_mem_free(PAL_MEM_INTERNAL);
    mem_t[1] = pal_sys_mem_total(PAL_MEM_EXTERNAL), mem_f[1] = pal_sys_mem_free(PAL_MEM_EXTERNAL);
    /* a process list is too big for the view's stack: it is counted from a heap copy */
    nema_proc_t *pl = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, NEMA_PROC_MAX * sizeof(*pl));
    procs = pl ? nema_proc_list(pl, NEMA_PROC_MAX) : 0;
    nema_free(pl);
}

static int cpu_get(void *ctx) { return cpu[(intptr_t)ctx]; }
static int mem_get(void *ctx) {
    int i = (int)(intptr_t)ctx;
    return mem_t[i] ? (int)((mem_t[i] - mem_f[i]) * 100 / mem_t[i]) : 0;
}
static void mem_text(void *ctx, char *out, size_t cap) {
    int i = (int)(intptr_t)ctx;
    size_t used = mem_t[i] - mem_f[i], total = mem_t[i];
    if (total >= 1024 * 1024) snprintf(out, cap, "%.1f of %.0f MB", (double)used / 1048576.0, (double)total / 1048576.0);
    else snprintf(out, cap, "%lu of %lu KB", (unsigned long)(used / 1024), (unsigned long)(total / 1024));
}
static void tasks_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%lu tasks, %lu processes", (unsigned long)nema_task_count(), (unsigned long)procs);
}

static const pal_menu_row_t ROWS[] = {
    {.kind = PAL_MENU_SECTION, .label = "Processor"},
    {.kind = PAL_MENU_SLIDER, .label = "Core 0 (%)", .get = cpu_get, .ctx = (void *)0, .min = 0, .max = 100, .step = 1},
    {.kind = PAL_MENU_SLIDER, .label = "Core 1 (%)", .get = cpu_get, .ctx = (void *)1, .min = 0, .max = 100, .step = 1},
    {.kind = PAL_MENU_SECTION, .label = "Memory"},
    {.kind = PAL_MENU_SLIDER, .label = "Internal RAM (%)", .get = mem_get, .ctx = (void *)0, .min = 0, .max = 100, .step = 1},
    {.kind = PAL_MENU_INFO, .label = "Internal used", .text = mem_text, .ctx = (void *)0, .inert = true},
    {.kind = PAL_MENU_SLIDER, .label = "PSRAM (%)", .get = mem_get, .ctx = (void *)1, .min = 0, .max = 100, .step = 1},
    {.kind = PAL_MENU_INFO, .label = "PSRAM used", .text = mem_text, .ctx = (void *)1, .inert = true},
    {.kind = PAL_MENU_SECTION, .label = "System"},
    {.kind = PAL_MENU_INFO, .label = "Running", .text = tasks_text, .inert = true},
};

static pal_menu_page_t PAGE = {.title = "Monitor", .rows = ROWS, .n = sizeof(ROWS) / sizeof(ROWS[0]), .live = true};
static pal_menu_t M;

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    pal_menu_draw(&M, c, a, pal_theme_active());
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    bool used = pal_menu_key(&M, k);
    if (used) pal_desktop_invalidate();
    return used;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    sample();
    pal_desktop_invalidate();
}

static void enter(pal_view_t *v) {
    (void)v;
    static bool inited;
    if (!inited) {
        inited = true;
        pal_menu_init(&M, &PAGE);
    }
    sample();
    pal_menu_bind("monitor", &M); /* the session lays the rows out in its own look */
}

pal_view_t pal_monitor_view = {.id = "monitor", .title = "Monitor", .enter = enter, .draw = draw, .key = key, .tick = tick, .tick_ms = 500};
