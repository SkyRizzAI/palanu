/* PalanuOS legend bar: the board's key caps for the semantic actions, the layout of the legends in the bar, and the
 * drawing of the bar. The desktop draws it after the layers (desktop.c); the content area above it shrinks by pal_legend_h_px().
 * The layout is pure, so the unit tests check it without a canvas. */
#include <string.h>

#include "nema/types.h"
#include "pal/board.h"
#include "pal/display.h"
#include "pal/legend.h"

#define EDGE 2 /* logical pixels from the bar's left and right ends */
#define GAP 6  /* the least space between two items */

const char *pal_legend_board_key(pal_legend_action_t a) {
    const pal_board_desc_t *b = pal_board_info();
    if (!b || !b->legend_keys || a <= PAL_ACT_NONE || a >= PAL_ACT_COUNT) return NULL;
    return b->legend_keys[a];
}

/* True when the first k items fit at the positions they would take; writes those positions into xs. */
static bool fits(const int *w, int k, int width, int *xs) {
    switch (k) {
    case 1:
        xs[0] = EDGE;
        return xs[0] + w[0] + EDGE <= width;
    case 2:
        xs[0] = EDGE;
        xs[1] = width - EDGE - w[1];
        return xs[0] + w[0] + GAP <= xs[1];
    case 3:
        xs[0] = EDGE;
        xs[2] = width - EDGE - w[2];
        xs[1] = (width - w[1]) / 2;
        return xs[0] + w[0] + GAP <= xs[1] && xs[1] + w[1] + GAP <= xs[2];
    default: {
        int x = EDGE;
        for (int i = 0; i < k; i++) {
            xs[i] = x;
            x += w[i] + GAP;
        }
        return x - GAP + EDGE <= width;
    }
    }
}

int pal_legend_layout(const int *widths, int n, int width, int *xs) {
    if (!widths || !xs || n <= 0 || width <= 0) return 0;
    if (n > PAL_LEGEND_MAX) n = PAL_LEGEND_MAX;
    for (int k = n; k > 0; k--)
        if (fits(widths, k, width, xs)) return k;
    return 0;
}

void pal_legend_draw(pal_canvas_t *c, const pal_legend_item_t *items, int n) {
    static kui_canvas_t L NEMA_EXT_BSS; /* the logical canvas of the bar, as kui_status.c's bar */
    const char *keys[PAL_LEGEND_MAX], *labels[PAL_LEGEND_MAX];
    int widths[PAL_LEGEND_MAX], xs[PAL_LEGEND_MAX], m = 0;
    for (int i = 0; i < n && items && m < PAL_LEGEND_MAX; i++) {
        const pal_legend_item_t *it = &items[i];
        if (it->action == PAL_ACT_NONE) continue;
        const char *key = it->key_text ? it->key_text : pal_legend_board_key(it->action);
        if (!key || !key[0]) continue; /* the board has no key for this action: the item is hidden */
        const char *label = it->label ? it->label : "";
        keys[m] = key;
        labels[m] = label;
        widths[m] = kui_legend_item_w(key, label);
        m++;
    }
    kui_canvas_init(&L, c->w, c->h, kui_scale_q());
    kui_clear(&L, false); /* paper: the bar shows nothing but its legends */
    int k = pal_legend_layout(widths, m, L.w, xs);
    int y = L.h - KUI_LEGEND_H;
    for (int i = 0; i < k; i++) kui_legend_item(&L, xs[i], y, keys[i], labels[i]);
    int h = pal_legend_h_px();
    kui_present(c, &L, (pal_rect_t){0, c->h - h, c->w, h});
}

/* ---- the text form (pal_legend_parse) ---- */

static const char *const ACT_NAMES[PAL_ACT_COUNT] = {
    [PAL_ACT_OK] = "ok",     [PAL_ACT_BACK] = "back",     [PAL_ACT_MOVE] = "move", [PAL_ACT_MOVE_H] = "move_h",
    [PAL_ACT_MOVE_V] = "move_v", [PAL_ACT_UP] = "up",     [PAL_ACT_DOWN] = "down", [PAL_ACT_TAB] = "tab",
    [PAL_ACT_MENU] = "menu", [PAL_ACT_DELETE] = "delete", [PAL_ACT_HOME] = "home",
};

#define LEGEND_CAP_MAX 15

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static bool name_is(const char *s, const char *name) {
    for (; *s && *name; s++, name++) {
        char c = *s >= 'A' && *s <= 'Z' ? (char)(*s - 'A' + 'a') : *s;
        if (c != *name) return false;
    }
    return !*s && !*name;
}

int pal_legend_parse(const char *spec, pal_legend_item_t *items, int max, char *buf, size_t cap) {
    if (!spec) return 0;
    size_t len = strlen(spec);
    if (!buf || len >= cap) return -1;
    memcpy(buf, spec, len + 1);
    int n = 0;
    char *p = buf;
    while (p) {
        char *next = strchr(p, ';');
        if (next) *next++ = 0;
        char *label = strchr(p, '=');
        if (label) *label++ = 0;
        char *name = trim(p);
        if (label) label = trim(label);
        p = next;
        if (!*name) {
            if (label && *label) return -1; /* "=label" names no key */
            continue;
        }
        if (n == max) return -1;
        pal_legend_item_t it = {PAL_ACT_NONE, label && *label ? label : NULL, NULL};
        size_t nl = strlen(name);
        if (name[0] == '[') {
            if (nl < 3 || name[nl - 1] != ']' || nl - 2 > LEGEND_CAP_MAX) return -1;
            name[nl - 1] = 0;
            it.action = PAL_ACT_CUSTOM;
            it.key_text = name + 1;
        } else {
            for (int a = PAL_ACT_OK; a < PAL_ACT_COUNT; a++)
                if (ACT_NAMES[a] && name_is(name, ACT_NAMES[a])) it.action = (pal_legend_action_t)a;
            if (it.action == PAL_ACT_NONE) return -1;
        }
        items[n++] = it;
    }
    return n;
}

int pal_legend_h_px(void) {
    pal_canvas_t *c = pal_display_canvas();
    return kui_bottom_px(c ? c->h : 240, KUI_LEGEND_H);
}
