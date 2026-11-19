/* PalanuOS perf: frame counters, key latency, boot stages and the perf overlay. See perf.h for the contract.
 *
 * Concurrency: the key path (input task) writes only the pending slot, and the display path (UI task) writes
 * everything else. All fields are 32-bit so loads and stores are single accesses; the worst case of a race is one
 * sample that is slightly off, which is acceptable for a meter.
 */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/perf.h"
#include "pal/theme.h"

#define KEY_PENDING_MAX_US 1000000u /* a key with no flush within 1 s is dropped, not timed */
#define OVERLAY_PERIOD_US 500000u   /* the overlay numbers change at most this often */
#define WINDOW_US 1000000u          /* fps and per-flush means use 1 s windows */
#define STALE_US 2000000u           /* after this long idle, fps reads 0 */
#define BOOT_MAX 8
#define OVERLAY_TEXT_MAX 32

/* ---- pure ring ---- */

void pal_perf_ring_push(pal_perf_ring_t *r, uint32_t us) {
    r->us[r->head] = us;
    r->head = (r->head + 1) % PAL_PERF_LAT_RING;
    if (r->n < PAL_PERF_LAT_RING) r->n++;
}

uint32_t pal_perf_ring_percentile(const pal_perf_ring_t *r, unsigned pct) {
    uint32_t sorted[PAL_PERF_LAT_RING];
    uint32_t n = r->n;
    if (n == 0) return 0;
    if (pct > 100) pct = 100;
    /* entries 0..n-1 are the valid ones: the ring fills sequentially before it wraps */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = r->us[i];
        uint32_t j = i;
        while (j > 0 && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    uint32_t rank = ((uint32_t)pct * n + 99u) / 100u; /* ceil(pct * n / 100) */
    if (rank == 0) rank = 1;
    return sorted[rank - 1];
}

uint32_t pal_perf_ring_max(const pal_perf_ring_t *r) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < r->n; i++)
        if (r->us[i] > m) m = r->us[i];
    return m;
}

/* ---- state ---- */

static struct {
    /* frames since reset */
    uint32_t flushes;
    uint64_t pixels;
    uint64_t draw_sum, flush_sum;
    uint32_t draw_last, flush_last, draw_max, flush_max;
    uint32_t pixels_last;
    /* 1 s window and the last completed one */
    bool win_open;
    uint32_t win_start, win_flushes, win_draw, win_flush;
    bool have_1s;
    uint32_t roll_us, fps, draw_1s, flush_1s;
    /* keys */
    bool key_pending;
    uint32_t key_t0, key_last_id, key_total;
    pal_perf_ring_t keys;
    /* overlay */
    bool ov_on, ov_painted;
    pal_rect_t ov_rect;
    char ov_shown[OVERLAY_TEXT_MAX];
    uint32_t ov_last_us;
} P NEMA_EXT_BSS; /* PSRAM: the internal RAM budget is spent (ramcheck) */

static struct {
    struct {
        const char *name;
        uint32_t ms;
    } stage[BOOT_MAX];
    int n;
} B;

static uint32_t now_us(void) { return (uint32_t)nema_port_time_us(); }

static uint32_t div_u32(uint64_t a, uint32_t b) { return b ? (uint32_t)(a / b) : 0; }

void pal_perf_flush_done(uint32_t draw_us, uint32_t flush_us, uint32_t pixels) {
    uint32_t now = now_us();
    P.flushes++;
    P.pixels += pixels;
    P.pixels_last = pixels;
    P.draw_sum += draw_us;
    P.flush_sum += flush_us;
    P.draw_last = draw_us;
    P.flush_last = flush_us;
    if (draw_us > P.draw_max) P.draw_max = draw_us;
    if (flush_us > P.flush_max) P.flush_max = flush_us;

    if (!P.win_open) {
        P.win_open = true;
        P.win_start = now;
    } else if (now - P.win_start >= WINDOW_US) {
        uint32_t span = now - P.win_start;
        P.fps = div_u32((uint64_t)P.win_flushes * 1000000u, span);
        P.draw_1s = div_u32(P.win_draw, P.win_flushes);
        P.flush_1s = div_u32(P.win_flush, P.win_flushes);
        P.have_1s = true;
        P.roll_us = now;
        P.win_start = now;
        P.win_flushes = P.win_draw = P.win_flush = 0;
    }
    P.win_flushes++;
    P.win_draw += draw_us;
    P.win_flush += flush_us;
}

void pal_perf_snapshot(pal_perf_stats_t *out) {
    uint32_t now = now_us();
    bool fresh = P.have_1s && (uint32_t)(now - P.roll_us) <= STALE_US;
    memset(out, 0, sizeof(*out));
    out->flushes = P.flushes;
    out->pixels_per_flush = div_u32(P.pixels, P.flushes);
    out->fps = fresh ? P.fps : 0;
    out->draw_us_1s = fresh ? P.draw_1s : 0;
    out->flush_us_1s = fresh ? P.flush_1s : 0;
    out->draw_us_mean = div_u32(P.draw_sum, P.flushes);
    out->flush_us_mean = div_u32(P.flush_sum, P.flushes);
    out->draw_us_last = P.draw_last;
    out->flush_us_last = P.flush_last;
    out->draw_us_max = P.draw_max;
    out->flush_us_max = P.flush_max;
}

void pal_perf_reset(void) {
    bool on = P.ov_on;
    memset(&P, 0, sizeof(P));
    P.ov_on = on;
}

/* ---- keys ---- */

void pal_perf_key_in(uint32_t id) {
    uint32_t now = now_us();
    if (!P.key_pending || (uint32_t)(now - P.key_t0) > KEY_PENDING_MAX_US) {
        P.key_pending = true;
        P.key_t0 = now;
    }
    P.key_last_id = id;
}

void pal_perf_key_shown(void) {
    if (!P.key_pending) return;
    P.key_pending = false;
    uint32_t d = now_us() - P.key_t0;
    if (d > KEY_PENDING_MAX_US) return;
    pal_perf_ring_push(&P.keys, d);
    P.key_total++;
}

uint32_t pal_perf_key_count(void) { return P.key_total; }
const pal_perf_ring_t *pal_perf_key_ring(void) { return &P.keys; }

/* ---- overlay ---- */

void pal_perf_overlay_set(bool on) {
    P.ov_on = on;
    P.ov_painted = false;
}
bool pal_perf_overlay_on(void) { return P.ov_on; }

static bool overlaps(pal_rect_t a, pal_rect_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool pal_perf_overlay_paint(pal_canvas_t *c, bool has_dirty, pal_rect_t dirty, pal_rect_t *out) {
    if (!P.ov_on || !c) return false;
    const pal_font_t *f = pal_theme_active()->tokens.caption;
    pal_rect_t r;
    r.w = pal_text_w(f, "d999/f999 999fps") + 6;
    r.h = f->h + 2;
    r.x = c->w - r.w - 2;
    r.y = c->h - r.h - 2;
    if (r.x < 0 || r.y < 0) return false;

    uint32_t now = now_us();
    pal_perf_stats_t s;
    pal_perf_snapshot(&s);
    char text[OVERLAY_TEXT_MAX];
    snprintf(text, sizeof(text), "d%lu/f%lu %lufps", (unsigned long)((s.draw_us_1s + 500u) / 1000u),
             (unsigned long)((s.flush_us_1s + 500u) / 1000u), (unsigned long)s.fps);

    bool moved = !P.ov_painted || memcmp(&r, &P.ov_rect, sizeof(r)) != 0;
    bool changed = strcmp(text, P.ov_shown) != 0;
    bool due = moved || (changed && (uint32_t)(now - P.ov_last_us) >= OVERLAY_PERIOD_US);
    bool touched = has_dirty && overlaps(dirty, r);
    if (!due && !touched) return false;

    if (due) {
        snprintf(P.ov_shown, sizeof(P.ov_shown), "%s", text);
        P.ov_last_us = now;
    }
    pal_clip_reset(c);
    pal_fill(c, r.x, r.y, r.w, r.h, PAL_C_INK);
    pal_text(c, f, r.x + 3, r.y + 1, P.ov_shown, PAL_C_PAPER, PAL_C_INK);
    P.ov_painted = true;
    P.ov_rect = r;
    *out = r;
    return true;
}

/* ---- boot stages ---- */

void pal_boot_mark_at(const char *name, uint32_t ms) {
    if (B.n >= BOOT_MAX) return;
    B.stage[B.n].name = name;
    B.stage[B.n].ms = ms;
    B.n++;
}

void pal_boot_mark(const char *name) { pal_boot_mark_at(name, (uint32_t)(nema_port_time_us() / 1000u)); }

size_t pal_boot_report(char *buf, size_t cap) {
    size_t n = 0;
    /* The first mark ("start", at the top of pal_boot) is the origin: the clock may count from the host's boot. */
    uint32_t base = B.n ? B.stage[0].ms : 0;
    uint32_t prev = 0;
    for (int i = 0; i < B.n; i++) {
        uint32_t ms = B.stage[i].ms - base;
        if (n < cap) {
            n += (size_t)snprintf(buf + n, cap - n, "%-10s %8lu ms  (+%lu)\n", B.stage[i].name,
                                  (unsigned long)ms, (unsigned long)(ms - prev));
        }
        prev = ms;
    }
    if (B.n == 0 && n < cap) n += (size_t)snprintf(buf + n, cap - n, "no boot stages recorded\n");
    return n;
}

/* The stages recorded so far, for the boot splash (ui_splash.c): their count and each name, in order. */
size_t pal_boot_stage_count(void) { return (size_t)B.n; }
const char *pal_boot_stage_name(size_t i) { return i < (size_t)B.n ? B.stage[i].name : ""; }
