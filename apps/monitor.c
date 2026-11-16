#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/sysinfo.h"
#include "pal/theme.h"

#define HIST 96

static uint8_t h0[HIST], h1[HIST];
static int hn;
static int cpu0, cpu1;
static pal_cpu_sample_t cpu_s; /* Monitor's own sample state; top keeps its own */

static void sample(void) {
    cpu0 = pal_sys_cpu_percent(&cpu_s, 0);
    cpu1 = pal_sys_cpu_count() > 1 ? pal_sys_cpu_percent(&cpu_s, 1) : 0;
    if (hn == HIST) {
        memmove(h0, h0 + 1, HIST - 1);
        memmove(h1, h1 + 1, HIST - 1);
        hn--;
    }
    h0[hn] = (uint8_t)cpu0;
    h1[hn] = (uint8_t)cpu1;
    hn++;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    sample();
    pal_desktop_invalidate();
}

static void fmt_used(char *buf, size_t n, size_t used, size_t total) {
    if (total >= 1024 * 1024) snprintf(buf, n, "%.1f/%.0fM", (double)used / 1048576.0, (double)total / 1048576.0);
    else snprintf(buf, n, "%lu/%luK", (unsigned long)(used / 1024), (unsigned long)(total / 1024));
}

static int gauge(pal_canvas_t *c, const pal_theme_t *t, int y, int w, const char *label, int pct, const char *val) {
    const pal_font_t *f = t->tokens.caption;
    int h = f->h + 3, px = t->tokens.pad_x;
    int lw = 5 * f->w + 4, vw = 12 * f->w;
    pal_text(c, f, px, y + 1, label, PAL_C_INK, PAL_TRANSPARENT);
    pal_text(c, f, w - px - pal_text_w(f, val), y + 1, val, PAL_C_INK, PAL_TRANSPARENT);
    int bx = px + lw, bw = w - px - vw - bx;
    pal_rect(c, bx, y + 2, bw, h - 3, PAL_C_INK);
    pal_fill(c, bx + 1, y + 3, (bw - 2) * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100, h - 5, PAL_C_ACCENT);
    return h + 2;
}

static void draw(pal_view_t *self, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)self, (void)full;
    const pal_theme_t *t = pal_theme_active();
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_PAPER);
    int y = a.y + t->comp.title_bar(c, t, a.y, "Monitor");
    char v[32];
    snprintf(v, sizeof(v), "%d%%", cpu0);
    y += gauge(c, t, y, a.w, "CPU0", cpu0, v);
    snprintf(v, sizeof(v), "%d%%", cpu1);
    y += gauge(c, t, y, a.w, "CPU1", cpu1, v);
    size_t it = pal_sys_mem_total(PAL_MEM_INTERNAL), ifr = pal_sys_mem_free(PAL_MEM_INTERNAL);
    fmt_used(v, sizeof(v), it - ifr, it);
    y += gauge(c, t, y, a.w, "RAM", it ? (int)((it - ifr) * 100 / it) : 0, v);
    size_t et = pal_sys_mem_total(PAL_MEM_EXTERNAL), efr = pal_sys_mem_free(PAL_MEM_EXTERNAL);
    fmt_used(v, sizeof(v), et - efr, et);
    y += gauge(c, t, y, a.w, "PSRAM", et ? (int)((et - efr) * 100 / et) : 0, v);
    const pal_font_t *f = t->tokens.caption;
    snprintf(v, sizeof(v), "tasks %lu   procs", (unsigned long)nema_task_count());
    /* only the count is shown: a process list is too big for the view's stack, so it is counted from a heap copy */
    nema_proc_t *pl = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, NEMA_PROC_MAX * sizeof(*pl));
    size_t procs = pl ? nema_proc_list(pl, NEMA_PROC_MAX) : 0;
    nema_free(pl);
    snprintf(v + strlen("tasks "), sizeof(v) - 6, "%lu  procs %lu", (unsigned long)nema_task_count(), (unsigned long)procs);
    pal_text(c, f, t->tokens.pad_x, y + 1, v, PAL_C_DIM, PAL_TRANSPARENT);
    y += f->h + 4;
    /* CPU history: core 0 solid, core 1 dotted */
    int gh = a.y + a.h - y - 3, gw = a.w - 2 * t->tokens.pad_x, gx = t->tokens.pad_x;
    if (gh > 12) {
        pal_rect(c, gx, y, gw, gh, PAL_C_DIM);
        int n = hn < gw - 2 ? hn : gw - 2;
        for (int core = 0; core < 2; core++) {
            const uint8_t *h = core ? h1 : h0;
            int prev = -1;
            for (int i = 0; i < n; i++) {
                int x = gx + gw - 1 - n + i;
                int yy = y + gh - 2 - h[hn - n + i] * (gh - 4) / 100;
                if (core == 0) {
                    if (prev >= 0) pal_line(c, x - 1, prev, x, yy, PAL_C_ACCENT);
                } else if (i & 1) {
                    pal_pixel(c, x, yy, PAL_C_INK);
                }
                prev = yy;
            }
        }
    }
}

static void enter(pal_view_t *v) {
    (void)v;
    sample();
}

pal_view_t pal_monitor_view = {.id = "monitor", .title = "Monitor", .enter = enter, .draw = draw, .tick = tick, .tick_ms = 500};
