/* PalanuOS overlay stack: the layers above the front view. The stack holds up to PAL_OVERLAY_MAX views. Each push
 * tells the layer below it (obscured), each removal of the top tells the new top (exposed), and every change redraws in full.
 * Desktop task only; the public calls post their work when they come from another task. */
#include <string.h>

#include "desktop_int.h"
#include "nema/nema.h"
#include "pal/overlay.h"

static struct {
    pal_view_t *v;
    uint32_t flags;
} S[PAL_OVERLAY_MAX];
static int n;

int ovl_count(void) { return n; }
pal_view_t *ovl_view(int i) { return i >= 0 && i < n ? S[i].v : NULL; }
uint32_t ovl_flags(int i) { return i >= 0 && i < n ? S[i].flags : 0; }
pal_view_t *ovl_top(void) { return n ? S[n - 1].v : NULL; }

int ovl_base(void) {
    for (int i = n - 1; i >= 0; i--)
        if (S[i].flags & PAL_VIEW_OPAQUE) return i;
    return -1;
}

static int find(const pal_view_t *v) {
    for (int i = 0; i < n; i++)
        if (S[i].v == v) return i;
    return -1;
}

nema_err_t ovl_push_now(pal_view_t *v, uint32_t extra_flags) {
    if (!v) return NEMA_ERR_INVAL;
    if (find(v) >= 0) return NEMA_ERR_EXIST;
    if (n == PAL_OVERLAY_MAX) return NEMA_ERR_FULL;
    pal_view_t *below = n ? S[n - 1].v : desk_front();
    if (below && below->obscured) below->obscured(below);
    S[n].v = v;
    S[n].flags = v->flags | extra_flags | PAL_VIEW_OVERLAY;
    n++;
    if (v->enter) v->enter(v);
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

void ovl_remove_now(pal_view_t *v) {
    int i = find(v);
    if (i < 0 || desk_locked_view(v)) return; /* the lock view leaves only through pal_desktop_unlock */
    bool was_top = i == n - 1;
    for (int j = i; j < n - 1; j++) S[j] = S[j + 1];
    n--;
    if (v->leave) v->leave(v);
    if (was_top) {
        pal_view_t *top = n ? S[n - 1].v : desk_front();
        if (top && top->exposed) top->exposed(top);
    }
    pal_desktop_invalidate_all();
}

void ovl_clear_now(void) {
    while (n) ovl_remove_now(S[n - 1].v);
}

/* ---- public API ---- */

static void post_push(void *arg) { ovl_push_now((pal_view_t *)arg, 0); }
static void post_pop(void *arg) {
    (void)arg;
    if (n) ovl_remove_now(S[n - 1].v);
}
static void post_remove(void *arg) { ovl_remove_now((pal_view_t *)arg); }

nema_err_t pal_desktop_overlay_push(pal_view_t *v) {
    if (!v) return NEMA_ERR_INVAL;
    if (!desk_started() || desk_on_task()) return ovl_push_now(v, 0);
    return desk_post(post_push, v);
}

void pal_desktop_overlay_pop(void) {
    if (!desk_started() || desk_on_task()) post_pop(NULL);
    else desk_post(post_pop, NULL);
}

void pal_desktop_overlay_remove(pal_view_t *v) {
    if (!v) return;
    if (!desk_started() || desk_on_task()) ovl_remove_now(v);
    else desk_post(post_remove, v);
}

pal_view_t *pal_desktop_overlay_top(void) { return ovl_top(); }
size_t pal_desktop_overlay_count(void) { return (size_t)n; }

