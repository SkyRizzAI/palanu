#include <string.h>

#include "nema/nema.h"
#include "pal/display.h"
#include "pal/perf.h"
#include "pal/theme.h"

static struct {
    pal_display_ops_t ops;
    pal_canvas_t canvas;
    uint16_t palette[256];
    int brightness;
    uint32_t flushes;
    bool ready;
    void (*tap)(void *ctx, pal_rect_t r);
    void *tap_ctx;
    int reserved_top, reserved_bottom;
    uint16_t *frame; /* the screen frame (RGB565, native order); NULL when the driver has no present_rgb565 */
    nema_port_mutex_t *flock;
} D;

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* An 8-bit rectangle through a palette (byte-swapped, the panel bus order) into the frame. */
static void to_frame(const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    nema_port_mutex_lock(D.flock);
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint8_t *src = px + (size_t)y * (size_t)stride + (size_t)r.x;
        uint16_t *dst = D.frame + (size_t)y * (size_t)D.ops.width + (size_t)r.x;
        for (int x = 0; x < r.w; x++) dst[x] = bswap16(pal[src[x]]);
    }
    nema_port_mutex_unlock(D.flock);
}

/* The rectangle of the frame to the panel, then to the mirror. */
static void send_frame(pal_rect_t r) {
    D.ops.present_rgb565(D.ops.ctx, D.frame, D.ops.width, r);
    D.flushes++;
    if (D.tap) D.tap(D.tap_ctx, r);
}

/* Every path to the panel: through the frame when there is one, else the 8-bit image straight to the driver. */
static void present_8(const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal, bool tap) {
    if (D.frame) {
        to_frame(px, stride, r, pal);
        send_frame(r);
        return;
    }
    D.ops.present(D.ops.ctx, px, stride, r, pal);
    D.flushes++;
    if (tap && D.tap) D.tap(D.tap_ctx, r);
}

nema_err_t pal_display_init(const pal_display_ops_t *ops) {
    memset(&D, 0, sizeof(D));
    D.ops = *ops;
    nema_owner_t owner = nema_owner_register("display");
    void *fb = nema_alloc_ex(NEMA_MEM_ANY, owner, (size_t)ops->width * (size_t)ops->height, 64, NEMA_ALLOC_ZERO);
    if (!fb) return NEMA_ERR_NOMEM;
    pal_canvas_init(&D.canvas, fb, ops->width, ops->height);
    if (ops->present_rgb565) {
        D.frame = nema_alloc_ex(NEMA_MEM_EXTERNAL, owner, (size_t)ops->width * (size_t)ops->height * 2u, 64, NEMA_ALLOC_ZERO);
        if (!D.frame) D.frame = nema_alloc_ex(NEMA_MEM_ANY, owner, (size_t)ops->width * (size_t)ops->height * 2u, 64, NEMA_ALLOC_ZERO);
        if (!D.frame) return NEMA_ERR_NOMEM;
    }
    D.flock = nema_port_mutex_create();
    D.brightness = 80;
    pal_theme_palette(D.palette);
    pal_clear(&D.canvas, PAL_C_PAPER);
    D.ready = true;
    return NEMA_OK;
}

pal_canvas_t *pal_display_canvas(void) { return &D.canvas; }

static int s_rotation;

int pal_display_rotation(void) { return s_rotation; }

bool pal_display_set_rotation(int degrees) {
    int deg = degrees == 180 ? 180 : 0;
    if (!D.ready || !D.ops.rotate) return deg == 0;
    if (deg == s_rotation) return true;
    nema_port_mutex_lock(D.flock);
    D.ops.rotate(D.ops.ctx, deg);
    s_rotation = deg;
    nema_port_mutex_unlock(D.flock);
    /* the panel holds the old picture the wrong way round: send all of it again */
    if (D.frame) send_frame((pal_rect_t){0, 0, D.ops.width, D.ops.height});
    pal_mark_all(&D.canvas);
    return true;
}

/* The perf overlay is painted on top of the composed frame and presented on its own small rectangle. Its paint must
 * not disturb the dirty state of the frame, so the dirty fields are saved and restored around it. */
static void overlay_present(bool any, pal_rect_t dirty) {
    pal_canvas_t *c = &D.canvas;
    int x0 = c->dirty_x0, y0 = c->dirty_y0, x1 = c->dirty_x1, y1 = c->dirty_y1;
    pal_rect_t o;
    if (pal_perf_overlay_paint(c, any, dirty, &o)) present_8(c->px, c->stride, o, D.palette, false);
    c->dirty_x0 = x0, c->dirty_y0 = y0, c->dirty_x1 = x1, c->dirty_y1 = y1;
}

int pal_display_flush(void) {
    pal_rect_t r = {0, 0, 0, 0};
    uint32_t t0 = (uint32_t)nema_port_time_us();
    if (!D.ready) return 0;
    bool any = pal_take_dirty(&D.canvas, &r);
    if (any && r.y < D.reserved_top) { /* the desktop's own bar is up there: never overwrite it */
        int cut = D.reserved_top - r.y;
        r.y += cut, r.h -= cut;
        any = r.h > 0;
    }
    int floor_y = D.canvas.h - D.reserved_bottom;
    if (any && D.reserved_bottom > 0 && r.y + r.h > floor_y) { /* nor its bar at the bottom */
        r.h = floor_y - r.y;
        any = r.h > 0;
    }
    int sent = 0;
    if (any) {
        uint32_t p0 = (uint32_t)nema_port_time_us();
        present_8(D.canvas.px, D.canvas.stride, r, D.palette, true);
        uint32_t p1 = (uint32_t)nema_port_time_us();
        uint32_t t1 = (uint32_t)nema_port_time_us();
        sent = r.w * r.h;
        pal_perf_flush_done(t1 - t0, p1 - p0, (uint32_t)sent);
        pal_perf_key_shown();
    }
    overlay_present(any, r);
    return sent;
}

void pal_display_refresh_theme(void) {
    if (!D.ready) return;
    pal_theme_palette(D.palette);
    pal_mark_all(&D.canvas);
}

void pal_display_set_brightness(int p) {
    D.brightness = p < 5 ? 5 : p > 100 ? 100 : p;
    if (D.ops.backlight) D.ops.backlight(D.ops.ctx, D.brightness);
}
int pal_display_brightness(void) { return D.brightness; }
void pal_display_backlight(bool on) {
    if (D.ops.backlight) D.ops.backlight(D.ops.ctx, on ? D.brightness : 0);
}
const uint16_t *pal_display_palette(void) { return D.palette; }
void pal_display_set_tap(void (*tap)(void *ctx, pal_rect_t r), void *ctx) {
    D.tap_ctx = ctx;
    D.tap = tap;
}
uint32_t pal_display_flush_count(void) { return D.flushes; }

bool pal_display_has_rgb565(void) { return D.ready && D.ops.present_rgb565 != NULL; }

/* A desktop that draws its own RGB565 frame (Athena) covers the perf overlay: paint it again over what was copied and
 * send both in one rectangle. */
static void send_frame_with_overlay(pal_rect_t r) {
    pal_canvas_t *c = &D.canvas;
    int x0 = c->dirty_x0, y0 = c->dirty_y0, x1 = c->dirty_x1, y1 = c->dirty_y1;
    pal_rect_t o;
    if (pal_perf_overlay_paint(c, true, r, &o)) {
        to_frame(c->px, c->stride, o, D.palette);
        int rx1 = r.x + r.w > o.x + o.w ? r.x + r.w : o.x + o.w, ry1 = r.y + r.h > o.y + o.h ? r.y + r.h : o.y + o.h;
        if (o.x < r.x) r.x = o.x;
        if (o.y < r.y) r.y = o.y;
        r.w = rx1 - r.x, r.h = ry1 - r.y;
    }
    c->dirty_x0 = x0, c->dirty_y0 = y0, c->dirty_x1 = x1, c->dirty_y1 = y1;
    send_frame(r);
}

bool pal_display_present_rgb565(const uint16_t *px, int stride, pal_rect_t r) {
    if (!pal_display_has_rgb565() || r.w <= 0 || r.h <= 0) return false;
    nema_port_mutex_lock(D.flock);
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(D.frame + (size_t)y * (size_t)D.ops.width + (size_t)r.x, px + (size_t)y * (size_t)stride + (size_t)r.x, (size_t)r.w * 2u);
    nema_port_mutex_unlock(D.flock);
    send_frame_with_overlay(r);
    return true;
}

void pal_display_present_indexed(const uint8_t *px, int stride, pal_rect_t r, const uint16_t *palette) {
    if (!D.ready || r.w <= 0 || r.h <= 0) return;
    present_8(px, stride, r, palette, true);
}

const uint16_t *pal_display_frame(void) { return D.frame; }

bool pal_display_blit_rgb565(const uint16_t *src, int stride, pal_rect_t r) {
    if (!D.frame || r.w <= 0 || r.h <= 0) return false;
    nema_port_mutex_lock(D.flock);
    for (int y = 0; y < r.h; y++)
        memcpy(D.frame + (size_t)(r.y + y) * (size_t)D.ops.width + (size_t)r.x, src + (size_t)y * (size_t)stride, (size_t)r.w * 2u);
    nema_port_mutex_unlock(D.flock);
    send_frame_with_overlay(r);
    return true;
}
void pal_display_frame_lock(bool on) {
    if (!D.flock) return;
    if (on) nema_port_mutex_lock(D.flock);
    else nema_port_mutex_unlock(D.flock);
}

void pal_display_reserve_top(int rows) { D.reserved_top = rows < 0 ? 0 : rows; }
void pal_display_reserve_bottom(int rows) { D.reserved_bottom = rows < 0 ? 0 : rows; }
