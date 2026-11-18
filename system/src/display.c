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
    int reserved_top;
} D;

nema_err_t pal_display_init(const pal_display_ops_t *ops) {
    memset(&D, 0, sizeof(D));
    D.ops = *ops;
    nema_owner_t owner = nema_owner_register("display");
    void *fb = nema_alloc_ex(NEMA_MEM_ANY, owner, (size_t)ops->width * (size_t)ops->height, 64, NEMA_ALLOC_ZERO);
    if (!fb) return NEMA_ERR_NOMEM;
    pal_canvas_init(&D.canvas, fb, ops->width, ops->height);
    D.brightness = 80;
    pal_theme_palette(D.palette);
    pal_clear(&D.canvas, PAL_C_PAPER);
    D.ready = true;
    return NEMA_OK;
}

pal_canvas_t *pal_display_canvas(void) { return &D.canvas; }

/* The perf overlay is painted on top of the composed frame and presented on its own small rectangle. Its paint must
 * not disturb the dirty state of the frame, so the dirty fields are saved and restored around it. */
static void overlay_present(bool any, pal_rect_t dirty) {
    pal_canvas_t *c = &D.canvas;
    int x0 = c->dirty_x0, y0 = c->dirty_y0, x1 = c->dirty_x1, y1 = c->dirty_y1;
    pal_rect_t o;
    if (pal_perf_overlay_paint(c, any, dirty, &o)) D.ops.present(D.ops.ctx, c->px, c->stride, o, D.palette);
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
    int sent = 0;
    if (any) {
        uint32_t p0 = (uint32_t)nema_port_time_us();
        D.ops.present(D.ops.ctx, D.canvas.px, D.canvas.stride, r, D.palette);
        uint32_t p1 = (uint32_t)nema_port_time_us();
        D.flushes++;
        if (D.tap) D.tap(D.tap_ctx, r);
        uint32_t t1 = (uint32_t)nema_port_time_us();
        sent = r.w * r.h;
        pal_perf_flush_done(t1 - t0, p1 - p0, (uint32_t)sent);
        pal_perf_key_shown();
    }
    overlay_present(any, r);
    return sent;
}

void pal_display_refresh_theme(void) {
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

bool pal_display_present_rgb565(const uint16_t *px, int stride, pal_rect_t r) {
    if (!pal_display_has_rgb565() || r.w <= 0 || r.h <= 0) return false;
    D.ops.present_rgb565(D.ops.ctx, px, stride, r);
    D.flushes++;
    return true;
}

void pal_display_present_indexed(const uint8_t *px, int stride, pal_rect_t r, const uint16_t *palette) {
    if (!D.ready || r.w <= 0 || r.h <= 0) return;
    D.ops.present(D.ops.ctx, px, stride, r, palette);
    D.flushes++;
}

void pal_display_reserve_top(int rows) { D.reserved_top = rows < 0 ? 0 : rows; }
