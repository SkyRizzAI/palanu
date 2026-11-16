/* PalanuOS app switcher: the views that were in front, most recent first, each with the memory its owner holds. Tap
 * the cycle key or Down to move, Enter to go there, Esc to close. The front view is the first line and is marked. Built-in
 * views run inside the desktop task, so their memory is the memory of the owner that carries their name (the Terminal's
 * owner is "terminal"); a view with no such owner shows a dash. */
#include <stdio.h>
#include <string.h>

#include "desktop_int.h"
#include "nema/mem.h"
#include "nema/nema.h"
#include "pal/overlay.h"
#include "pal/theme.h"

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full);
static bool key(pal_view_t *v, const pal_key_t *k);

static const pal_legend_item_t LEGENDS[] = {{PAL_ACT_OK, "open", NULL}, {PAL_ACT_BACK, "close", NULL}};

static struct {
    pal_view_t view;
    int sel, top, vis;
} SW = {.view = {.id = "switcher", .title = "Apps", .flags = PAL_VIEW_NOBAR, .draw = draw, .key = key, .data = &SW,
                 .legends = LEGENDS, .nlegends = 2}};

static uint32_t owner_bytes(const char *name) {
    for (int o = 0; o < NEMA_OWNER_MAX; o++) {
        nema_owner_stat_t st;
        if (!nema_mem_owner_stat((nema_owner_t)o, &st) || strcmp(st.name, name) != 0) continue;
        uint32_t s = 0;
        for (int k = 0; k < NEMA_MEM_CLASS_COUNT; k++) s += st.used[k];
        return s;
    }
    return 0;
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_PAPER);
    int y = a.y + t->comp.title_bar(c, t, a.y, "Apps");
    int rh = t->comp.row_height(t);
    int n = desk_mru_count();
    SW.vis = (a.y + a.h - y - 2) / rh;
    if (SW.vis < 1) SW.vis = 1;
    if (SW.sel >= n) SW.sel = n ? n - 1 : 0;
    if (SW.sel < SW.top) SW.top = SW.sel;
    if (SW.sel >= SW.top + SW.vis) SW.top = SW.sel - SW.vis + 1;
    bool bar = n > SW.vis;
    for (int i = SW.top; i < n && i < SW.top + SW.vis; i++) {
        pal_view_t *e = desk_mru_at(i);
        char val[16];
        uint32_t b = owner_bytes(e->id);
        if (i == 0) snprintf(val, sizeof(val), "front");
        else if (b) snprintf(val, sizeof(val), "%luK", (unsigned long)(b / 1024));
        else snprintf(val, sizeof(val), "-");
        t->comp.list_row(c, t, 0, y + (i - SW.top) * rh, a.w - (bar ? 6 : 0), e->title ? e->title : e->id, val,
                         i == SW.sel);
    }
    if (bar) t->comp.scrollbar(c, t, a.w - 4, y, SW.vis * rh, SW.top, SW.vis, n);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    int n = desk_mru_count();
    if (!n) return false;
    switch (k->sym) {
    case PAL_KEY_UP:
    case PAL_KEY_LEFT: SW.sel = (SW.sel + n - 1) % n; break;
    case PAL_KEY_DOWN:
    case PAL_KEY_RIGHT:
    case PAL_KEY_SWITCH: SW.sel = (SW.sel + 1) % n; break;
    case PAL_KEY_ENTER: {
        pal_view_t *e = desk_mru_at(SW.sel);
        if (e) pal_desktop_open(e->id); /* closes this overlay too */
        return true;
    }
    case PAL_KEY_ESC: return false; /* the desktop pops the overlay */
    default: return true;
    }
    pal_desktop_invalidate();
    return true;
}

nema_err_t pal_switcher_open(void) {
    SW.sel = desk_mru_count() >= 2 ? 1 : 0; /* the previous app is what a tap of the cycle key goes to */
    SW.top = 0;
    return ovl_push_now(&SW.view, 0);
}

static void post_switcher(void *arg) {
    (void)arg;
    if (ovl_count() == 0 || ovl_view(ovl_count() - 1) != &SW.view) pal_switcher_open();
}

void pal_desktop_switcher_show(void) {
    if (!desk_started() || desk_on_task()) post_switcher(NULL);
    else desk_post(post_switcher, NULL);
}
