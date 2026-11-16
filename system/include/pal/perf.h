/* PalanuOS perf: display frame counters, key latency, boot stage timing, and the on-screen perf overlay.
 *
 * Why: frame cost, key-to-screen delay and boot time were guessed until now. These counters are fixed-size and
 * cheap enough to leave on, so they can be read from the shell (perf, perf keys, boot, mem) or the screen (overlay).
 * Nothing here allocates.
 */
#ifndef PAL_PERF_H
#define PAL_PERF_H

#include <stddef.h>
#include <stdint.h>

#include "pal/gfx.h"

/* ---- latency ring (pure; unit tested) ---- */
#define PAL_PERF_LAT_RING 128

typedef struct {
    uint32_t us[PAL_PERF_LAT_RING]; /* microseconds, oldest overwritten first */
    uint32_t n;                     /* valid entries, at most PAL_PERF_LAT_RING */
    uint32_t head;                  /* next write index */
} pal_perf_ring_t;

void pal_perf_ring_push(pal_perf_ring_t *r, uint32_t us);
/* Nearest-rank percentile over the valid entries, pct in 0..100. Returns 0 when empty. */
uint32_t pal_perf_ring_percentile(const pal_perf_ring_t *r, unsigned pct);
uint32_t pal_perf_ring_max(const pal_perf_ring_t *r);

/* ---- frame counters ---- */
typedef struct {
    uint32_t flushes;                   /* flushes since reset */
    uint32_t pixels_per_flush;          /* mean since reset */
    uint32_t fps;                       /* flushes in the last full second (0 when idle for 2 s) */
    uint32_t draw_us_1s, flush_us_1s;   /* mean per flush over the last full second */
    uint32_t draw_us_mean, flush_us_mean; /* mean since reset */
    uint32_t draw_us_last, flush_us_last;
    uint32_t draw_us_max, flush_us_max;
} pal_perf_stats_t;

/* draw = the whole pal_display_flush call that sent a frame (dirty take, panel transfer, tap);
 * flush = the panel transfer alone. Composition happens in the desktop and is not timed here. */
void pal_perf_flush_done(uint32_t draw_us, uint32_t flush_us, uint32_t pixels);
void pal_perf_snapshot(pal_perf_stats_t *out);
void pal_perf_reset(void); /* counters and latency ring; boot stages and overlay state are kept */

/* ---- key latency: from entering the input pipeline to the next completed flush ---- */
void pal_perf_key_in(uint32_t id);
void pal_perf_key_shown(void);
uint32_t pal_perf_key_count(void); /* key latencies recorded since reset */
const pal_perf_ring_t *pal_perf_key_ring(void);

/* ---- perf overlay ---- */
void pal_perf_overlay_set(bool on); /* the caller redraws the screen when turning it off */
bool pal_perf_overlay_on(void);
/* Called by the display after a flush. Paints the overlay into c and returns true with its rectangle when the
 * caller must present it: when the numbers changed (at most every 500 ms) or when the last flush touched it. */
bool pal_perf_overlay_paint(pal_canvas_t *c, bool has_dirty, pal_rect_t dirty, pal_rect_t *out);

/* ---- boot stages ---- */
void pal_boot_mark(const char *name);          /* records uptime in ms under name (a static string) */
void pal_boot_mark_at(const char *name, uint32_t ms); /* same with an explicit clock, for tests */
size_t pal_boot_report(char *buf, size_t cap); /* one line per stage; returns length */
size_t pal_boot_stage_count(void);             /* the stages recorded so far (the boot splash shows them) */
const char *pal_boot_stage_name(size_t i);      /* the name of stage i, "" past the end */

#endif
