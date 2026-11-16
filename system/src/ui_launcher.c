/* The launcher skins' glue: the registry entries become kui items (label, icon bitmap) and the skin for ui.launcher draws them.
 * The drawing is in kui_launcher.c. The list skin takes the 16 px icons (its rows are 15 px), the others the 32 px ones,
 * scaled by the skin as the reference scales its icons. */
#include <string.h>

#include "pal/icons.h"
#include "pal/kui.h"
#include "pal/ui.h"
#include "ui_launcher.h"

const char *ui_entry_icon(const pal_reg_app_t *e) { return e->icon[0] ? e->icon : e->id; }

static const pal_icon_t *icon_for(const char *name, int size) {
    const pal_icon_t *ic = ui_fixture_icon(name);
    if (!ic) ic = pal_icon_find(name, size);
    if (!ic) ic = pal_icon_find("default", size);
    return ic;
}

static void empty(kui_canvas_t *k) {
    const char *msg = "No apps";
    kui_text(k, &FONT_REG8, (k->w - kui_text_w(&FONT_REG8, msg)) / 2, k->clip_y0 + 4, msg, true);
}

void ui_skin_draw(kui_canvas_t *k, int style, const ui_entries_t *e) {
    if (e->n <= 0) {
        empty(k);
        return;
    }
    int size = style == PAL_UI_LAUNCHER_LIST ? 16 : 32;
    kui_item_t it[UI_LAUNCHER_MAX];
    int n = e->n < UI_LAUNCHER_MAX ? e->n : UI_LAUNCHER_MAX;
    for (int i = 0; i < n; i++) {
        const pal_reg_app_t *en = &e->v[i];
        const pal_icon_t *ic = icon_for(ui_entry_icon(en), size);
        it[i].label = en->name[0] ? en->name : en->id;
        it[i].bits = ic ? ic->bits : NULL;
        it[i].w = ic ? ic->w : 0;
        it[i].h = ic ? ic->h : 0;
        const pal_icon_t *sm = icon_for(ui_entry_icon(en), 16); /* the strip neighbours' 16 px icon */
        it[i].small_bits = sm ? sm->bits : NULL;
        it[i].small_w = sm ? sm->w : 0;
        it[i].small_h = sm ? sm->h : 0;
    }
    switch (style) {
    case PAL_UI_LAUNCHER_GRID: kui_skin_grid(k, it, n, e->sel); break;
    case PAL_UI_LAUNCHER_COMPACT: kui_skin_compact(k, it, n, e->sel); break;
    case PAL_UI_LAUNCHER_LIST: kui_skin_list(k, it, n, e->sel); break;
    default: kui_skin_strip(k, e->title, it, n, e->sel); break;
    }
}
