/* PalanuOS menu pages: drawing (pal/menu.h) in the layout of the pre-port Settings (commit 9471f26): an inverted title bar under
 * the status bar, section headers in small capitals with a rule, rows with 8 px icons at the left, the selected row inverted
 * on a rounded bar, and a scrollbar when rows are cut off. Drawing goes through kui (logical 1-bit canvas, 2 px per pixel) in
 * The reference fonts, so the style is the current one. The page draws no footer: it gives the system legend bar (pal/legend.h) the
 * keys of the selected row. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/icons.h"
#include "pal/kui.h"
#include "pal/legend.h"
#include "pal/menu.h"
#include "pal/ui.h"

#define ROW_H 13      /* REG8 cell (11) + 2 */
#define ROW_GAP 1     /* the gap between rows */
#define SECTION_H 12  /* a section header with its gap: caps (8) and a rule, 4 px of space above */
#define SECTION_BASE 11
#define LIST_PAD 2    /* the list's inset from its top, left and right edges */
#define TITLE_TOP 1   /* the title band starts one row below the status bar (its top is the area's top + 1) */
#define TITLE_H 13    /* the inverted band: BOLD8 cell (11) + 2 */
#define TITLE_GAP 3   /* between the band and the list */
#define GAUGE_H 7     /* The reference ProgressBar height */
#define GAUGE_GAP 3   /* The reference ProgressBar bottom margin */
#define ICON_COL 10   /* an 8 px icon and its gap: the label column of a page with icons */
#define ICON_X 4      /* the icon's inset from the row's left edge */
#define BAR_X (k->w - 3) /* the scrollbar's x (logical): the 3 px bar at the right edge, at any UI scale */
#define BOTTOM_PAD 1  /* the list's bottom inset (the view rect's last row) */

static kui_canvas_t S_KC NEMA_EXT_BSS;
static int S_LIST_H = 78; /* the list height of the last draw: pal_menu_follow scrolls for it */
static int S_LIST_W = 156; /* and its width (the canvas width less 4: the width changes with the UI scale) */

/* The legend bar's items for the selected row (pal/legend.h): what OK does on this row kind, and Back (or Home at the root),
 * as the pre-port footer had them. Two buffers take turns, so pal_legend_set sees a new array whenever the items change. */
static pal_legend_item_t S_LEG[2][2];
static int S_LEG_CUR = -1, S_LEG_N;

static const char *ok_label(const pal_menu_row_t *r) {
    if (!r || r->disabled) return NULL;
    switch (r->kind) {
    case PAL_MENU_SUBMENU: return "Open";
    case PAL_MENU_TOGGLE: return "Toggle";
    case PAL_MENU_CHOICE: return "Change";
    case PAL_MENU_ACTION: return "Run";
    case PAL_MENU_TEXT: return "Edit";
    default: return NULL;
    }
}

static void set_legends(pal_menu_t *m, const pal_menu_page_t *p) {
    pal_legend_item_t it[2];
    int n = 0;
    const pal_menu_row_t *r = (p && p->n > 0 && p->sel >= 0 && (size_t)p->sel < p->n) ? &p->rows[p->sel] : NULL;
    const char *ok = ok_label(r);
    if (ok) it[n++] = (pal_legend_item_t){PAL_ACT_OK, ok, NULL};
    it[n++] = (pal_legend_item_t){PAL_ACT_BACK, pal_menu_depth(m) > 1 ? "Back" : "Home", NULL};
    if (S_LEG_CUR >= 0 && n == S_LEG_N && !memcmp(it, S_LEG[S_LEG_CUR], sizeof(it[0]) * (size_t)n)) return;
    S_LEG_CUR = S_LEG_CUR == 0 ? 1 : 0;
    memcpy(S_LEG[S_LEG_CUR], it, sizeof(it[0]) * (size_t)n);
    S_LEG_N = n;
    pal_legend_set(S_LEG[S_LEG_CUR], n);
}

/* The value as it fits maxw: the whole text, or its longest prefix that fits with "..." after it. */
static void text_in(kui_canvas_t *k, const kui_font_t *f, int x, int y, int w, const char *s, bool selected);

static void fit_value(const kui_font_t *f, const char *s, int maxw, char *out, size_t cap) {
    snprintf(out, cap, "%s", s);
    if (kui_text_w(f, out) <= maxw) return;
    size_t n = strlen(s);
    while (n > 0) {
        n--;
        snprintf(out, cap, "%.*s...", (int)n, s);
        if (kui_text_w(f, out) <= maxw) return;
    }
    out[0] = 0;
}

/* A row's label in avail pixels at the row's label inset: cut with "...", or scrolling on the selected row. */
static void row_label(kui_canvas_t *k, int lx, int y, int h, int avail, const char *label, bool selected) {
    const kui_font_t *f = kui_slot(KUI_F_REG8);
    text_in(k, f, lx + 5, y + (h - f->charH) / 2, avail, label ? label : "", selected);
}

static void value_of(const pal_menu_row_t *r, char *out, size_t cap) {
    out[0] = 0;
    if (r->text) r->text(r->ctx, out, cap);
}

static bool page_has_icons(const pal_menu_page_t *p) {
    for (size_t i = 0; i < p->n; i++)
        if (p->rows[i].icon) return true;
    return false;
}

/* The label column's offset: a page with icons indents every row by ICON_COL, as the old pages did. */
static int row_off(const pal_menu_page_t *p) { return page_has_icons(p) ? ICON_COL : 0; }

/* The next line of a wrapped value, searching from byte off in v (w pixels wide): the line is v[*start, *start + *len) and the
 * next line is searched from *next. A line breaks at the last space that fits, or at a newline; a word longer than a line is
 * cut where it overflows, at least one character per line. Returns false at the end of the text. */
static bool wrap_next(const char *v, size_t off, int w, size_t *start, size_t *len, size_t *next) {
    size_t n = strlen(v);
    while (off < n && v[off] == ' ') off++; /* no spaces at the start of a line */
    if (off >= n) return false;
    *start = off;
    const kui_font_t *f = kui_slot(KUI_F_REG8);
    size_t k = 1, brk = 0; /* brk: the length up to the last space seen (0: none) */
    for (;;) {
        if (off + k > n) { *len = n - off, *next = n; return true; }
        char c = v[off + k - 1];
        if (c == '\n') { *len = k - 1, *next = off + k; return true; }
        if (c == ' ') brk = k - 1;
        char tmp[PAL_MENU_VALUE_MAX];
        if (k >= sizeof(tmp)) { *len = k - 1, *next = off + k - 1; return true; }
        memcpy(tmp, v + off, k);
        tmp[k] = 0;
        if (kui_text_w(f, tmp) > w && k > 1) {
            if (brk > 0) *len = brk, *next = off + brk + 1; /* break at the space; it is dropped */
            else *len = k - 1, *next = off + k - 1;         /* one long word: cut it */
            return true;
        }
        k++;
    }
}

/* Lines a wrapped INFO value takes in w pixels (at least one). */
static int wrap_count(const char *v, int w) {
    int lines = 0;
    size_t off = 0, st, len, next;
    while (wrap_next(v, off, w, &st, &len, &next)) {
        off = next;
        lines++;
    }
    return lines ? lines : 1;
}

int pal_menu_row_h(const pal_menu_page_t *p, size_t i, const pal_theme_t *t, int w) {
    (void)t;
    const pal_menu_row_t *r = &p->rows[i];
    if (pal_menu_row_hidden(r)) return 0;
    if (r->kind == PAL_MENU_SECTION) return SECTION_H;
    if (r->kind == PAL_MENU_INFO && r->wrap && r->text) {
        char v[PAL_MENU_VALUE_MAX];
        value_of(r, v, sizeof(v));
        return ROW_H + ROW_GAP + kui_slot(KUI_F_REG8)->charH * wrap_count(v, w - 2 * 5 - row_off(p));
    }
    if (pal_menu_gauge(r)) return GAUGE_H + GAUGE_GAP + ROW_GAP; /* The reference ProgressBar: 7 px bar, 3 px below */
    return ROW_H + ROW_GAP;
}

/* The scroll is in pixels (the reference ScrollState + scrollIntoView): the viewport is the list, the content is LIST_PAD, the rows
 * with their gaps, and LIST_PAD. When the selection moves outside the viewport the scroll moves the least that shows it. */
static int row_top(const pal_menu_page_t *p, size_t i, const pal_theme_t *t, int w) {
    int y = LIST_PAD;
    for (size_t j = 0; j < i; j++) y += pal_menu_row_h(p, j, t, w);
    return y;
}

static int content_h(const pal_menu_page_t *p, const pal_theme_t *t, int w) {
    int y = 2 * LIST_PAD;
    for (size_t j = 0; j < p->n; j++) y += pal_menu_row_h(p, j, t, w);
    return y;
}

void pal_menu_layout(pal_menu_t *m, const pal_theme_t *t, int w, int list_h, int *first, int *count) {
    pal_menu_page_t *p = pal_menu_top(m);
    *first = 0;
    *count = 0;
    S_LIST_W = w, S_LIST_H = list_h; /* what pal_menu_scroll and pal_menu_reveal measure against: the list as last laid out */
    if (!p || p->n == 0) return;
    if ((size_t)p->sel >= p->n) p->sel = (int)p->n - 1;
    int view = list_h;
    int cont = content_h(p, t, w);
    int maxs = cont > view ? cont - view : 0;
    if (p->follow && p->sel >= 0 && !pal_menu_row_hidden(&p->rows[p->sel]) && pal_menu_selectable(&p->rows[p->sel])) {
        int top = row_top(p, (size_t)p->sel, t, w);
        int bottom = top + ROW_H;
        /* The section headers right above the row come with it. */
        int want = top;
        for (int j = p->sel - 1; j >= 0 && (pal_menu_row_hidden(&p->rows[j]) || p->rows[j].kind == PAL_MENU_SECTION); j--)
            if (!pal_menu_row_hidden(&p->rows[j])) want = row_top(p, (size_t)j, t, w);
        if (bottom - want > view) want = bottom - view; /* a tall block: at least the row itself */
        if (want < p->spx) p->spx = want;
        else if (bottom > p->spx + view) p->spx = bottom - view;
        /* The ends. Arriving at the first selectable row shows what is above it, when that fits with the row; arriving at the
         * last one shows what is below it, as far as the row (with its headers) stays in view. What does not fit is read with
         * Up and Down (pal_menu_reveal). */
        size_t lo = p->n, hi = 0;
        for (size_t i = 0; i < p->n; i++)
            if (pal_menu_selectable(&p->rows[i])) {
                if (lo == p->n) lo = i;
                hi = i;
            }
        if ((size_t)p->sel == lo && bottom <= view) p->spx = 0;
        else if ((size_t)p->sel == hi) {
            int to = maxs < want ? maxs : want;
            if (p->spx < to) p->spx = to;
        }
    }
    p->follow = false; /* only once per selection change: a key that scrolls to read rows must not be undone next frame */
    if (p->spx > maxs) p->spx = maxs;
    if (p->spx < 0) p->spx = 0;
    for (size_t i = 0; i < p->n; i++) {
        int top = row_top(p, i, t, w);
        int rh = pal_menu_row_h(p, i, t, w);
        if (rh <= 0) continue;
        if (top + rh <= p->spx) continue;
        if (*count == 0) *first = (int)i;
        if (top >= p->spx + view) break;
        (*count)++;
    }
}

static int choice_count(const pal_menu_row_t *r) {
    int n = 0;
    while (r->choices && r->choices[n]) n++;
    return n;
}

/* The 8 px icon of a row: the bitmap of that name at 8 px when there is one, else the 16 px bitmap sampled down (a 2x2 block
 * is ink when at least two of its pixels are). Both are PalanuOS's own art (assets/icons). */
void pal_menu_icon8(kui_canvas_t *k, const char *name, int x, int y) {
    const pal_icon_t *ic = pal_icon_find(name, 8);
    if (ic) {
        kui_bitmap(k, x, y, 8, 8, ic->bits, true);
        return;
    }
    ic = pal_icon_find(name, 16);
    if (!ic) return;
    int bpr = (ic->w + 7) / 8;
    for (int r = 0; r < 8; r++)
        for (int col = 0; col < 8; col++) {
            int n = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int sy = 2 * r + dy, sx = 2 * col + dx;
                    if (ic->bits[sy * bpr + sx / 8] & (0x80 >> (sx & 7))) n++;
                }
            if (n >= 1) kui_pixel(k, x + col, y + r, true);
        }
}

/* Section header: the title in small capitals (FONT_6X8) at x+5 and a rule to the right edge, as the old pages. */
static void draw_section(kui_canvas_t *k, int x, int y, int w, const char *label) {
    char up[48];
    size_t n = 0;
    for (const char *s = label ? label : ""; *s && n + 1 < sizeof(up); s++)
        up[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    up[n] = 0;
    int tx = x + 5, tw = kui_text_w(&FONT_6X8, up);
    kui_text(k, &FONT_6X8, tx, y + 1, up, true);
    int lx = tx + tw + 4, rx = x + w - 5;
    if (rx > lx) kui_fill(k, lx, y + 5, rx - lx, 1, true);
}

/* One row at logical (x, y), width w, ROW_H tall. Content is drawn in ink first; the selected box then inverts it. */
static void draw_row(kui_canvas_t *k, pal_menu_page_t *p, size_t i, int x, int y, int w, bool selected) {
    const pal_menu_row_t *r = &p->rows[i];
    const int h = ROW_H;
    const int off = row_off(p);
    if (r->kind == PAL_MENU_SECTION) {
        draw_section(k, x, y, w, r->label);
        return;
    }
    if (r->icon && off) pal_menu_icon8(k, r->icon, x + ICON_X, y + (h - 8) / 2);
    const int lx = x + off, lw = w - off; /* the label column: what the icon column leaves */
    if (r->kind == PAL_MENU_INFO && r->wrap && r->text) {
        /* label on the first line, the value wrapped under it (the reference multi-line text, at the label's inset) */
        char v[PAL_MENU_VALUE_MAX];
        value_of(r, v, sizeof(v));
        row_label(k, lx, y, h, lw - 10, r->label, selected);
        int ly = y + h;
        size_t o = 0, st, len, next;
        while (wrap_next(v, o, lw - 10, &st, &len, &next)) {
            char line[PAL_MENU_VALUE_MAX];
            memcpy(line, v + st, len);
            line[len] = 0;
            kui_text(k, kui_slot(KUI_F_REG8), lx + 5, ly, line, true);
            ly += kui_slot(KUI_F_REG8)->charH;
            o = next;
        }
        return;
    }
    if (pal_menu_gauge(r)) { /* The reference drawRoundRect track (2 px inset from the row), filled to pct with a 2 px inset */
        int pct = r->get ? r->get(r->ctx) : r->min;
        int span = r->max - r->min;
        pct = span > 0 ? (pct - r->min) * 100 / span : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int bx = lx + 5, bw = lw - 10;
        if (bw < 4) return;
        kui_round(k, bx, y, bw, GAUGE_H, 2, true);
        int fw = (bw - 4) * pct / 100;
        if (fw > 0) kui_fill_round(k, bx + 2, y + 2, fw, GAUGE_H - 4, 1, true);
        return;
    }
    const int right = lx + lw - 7; /* the value, chevron and switch sit 5 px inside, plus the 2 px gap */
    bool sel_box = selected && pal_menu_selectable(r); /* a disabled row shows the cursor too: Enter says why it is off */
    char val[PAL_MENU_VALUE_MAX], shown[PAL_MENU_VALUE_MAX];
    switch (r->kind) {
    case PAL_MENU_CHOICE: {
        int cur = r->get ? r->get(r->ctx) : 0, n = choice_count(r);
        const char *lab = (cur >= 0 && cur < n) ? r->choices[cur] : "";
        /* "< value >" at the right, the value centred between the chevrons' inset and the label; the label takes what is left */
        const kui_font_t *f = kui_slot(KUI_F_REG8);
        int ty = y + (h - f->charH) / 2, cw = kui_text_w(f, "<"), vw = kui_text_w(f, lab);
        int gx = lx + lw - 5 - cw, vr = gx - 4, vl = vr - vw, lxc = vl - 4 - cw;
        row_label(k, lx, y, h, lxc - 3 - (lx + 5), r->label, selected);
        kui_text(k, f, lxc, ty, "<", true);
        kui_text(k, f, vl, ty, lab, true);
        kui_text(k, f, gx, ty, ">", true);
        break;
    }
    case PAL_MENU_TOGGLE: {
        bool on = r->get ? r->get(r->ctx) != 0 : false;
        row_label(k, lx, y, h, lw - 2 * 5 - 18 - 2, r->label, selected);
        kui_row_switch(k, lx, y, h, lw, on);
        break;
    }
    case PAL_MENU_SUBMENU:
    case PAL_MENU_INFO:
    case PAL_MENU_TEXT:
    case PAL_MENU_SLIDER:
    case PAL_MENU_ACTION: {
        int edge = right;
        if (r->kind == PAL_MENU_SUBMENU || r->kind == PAL_MENU_TEXT) { /* a TEXT row opens the keyboard: a chevron, as the reference nav rows */
            kui_row_chevron(k, edge, y, h, true);
            edge -= kui_text_w(kui_slot(KUI_F_REG8), ">") + 2;
        }
        value_of(r, val, sizeof(val));
        if (val[0] && (r->kind != PAL_MENU_ACTION || r->show_value)) {
            const kui_font_t *f = kui_slot(KUI_F_REG8);
            /* the label keeps its width; the value takes what is left (at least a third of the row) and is cut with "..." */
            int avail = edge - (lx + 5), lwid = kui_text_w(f, r->label ? r->label : "");
            int vmax = avail - lwid - 6;
            if (vmax < avail / 3) vmax = avail / 3;
            if (selected && kui_text_w(f, val) > vmax) { /* too long for its place: scroll it on the selected row */
                text_in(k, f, edge - vmax, y + (h - f->charH) / 2, vmax, val, true);
                edge -= vmax + 2;
            } else {
                fit_value(f, val, vmax, shown, sizeof(shown));
                int vw = kui_text_w(f, shown);
                kui_text(k, f, edge - vw, y + (h - f->charH) / 2, shown, true);
                edge -= vw + 2;
            }
        }
        row_label(k, lx, y, h, edge - (lx + 5) - 2, r->label, selected);
        break;
    }
    default:
        break;
    }
    kui_row_frame(k, x, y, w, h, sel_box);
}

bool pal_menu_reveal(pal_menu_t *m, int dir) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p || p->n == 0 || (dir != 1 && dir != -1)) return false;
    if (p->sel < 0 || (size_t)p->sel >= p->n || !pal_menu_selectable(&p->rows[p->sel])) return false;
    /* The rows between the selection and the next selectable one: the far one that is not a header (a header comes with the
     * row below it). With no selectable row left in that direction, up to the page's end: then every row counts, headers too,
     * since nothing will bring them into view. */
    int edge = -1, any = -1;
    bool found = false;
    for (int j = p->sel + dir; j >= 0 && j < (int)p->n; j += dir) {
        const pal_menu_row_t *r = &p->rows[j];
        if (pal_menu_selectable(r)) {
            found = true;
            break;
        }
        if (pal_menu_row_hidden(r)) continue;
        any = j;
        if (r->kind != PAL_MENU_SECTION) edge = j;
    }
    if (!found && any >= 0) edge = any;
    if (edge < 0) return false;
    int top = row_top(p, (size_t)edge, NULL, S_LIST_W), bot = top + pal_menu_row_h(p, (size_t)edge, NULL, S_LIST_W);
    int cont = content_h(p, NULL, S_LIST_W), maxs = cont > S_LIST_H ? cont - S_LIST_H : 0;
    int step = ROW_H + ROW_GAP;
    if (dir > 0 && bot > p->spx + S_LIST_H && p->spx < maxs) {
        p->spx += step;
        if (p->spx > maxs) p->spx = maxs;
        return true;
    }
    if (dir < 0 && top < p->spx && p->spx > 0) {
        p->spx -= step;
        if (p->spx < 0) p->spx = 0;
        return true;
    }
    return false;
}

bool pal_menu_scroll(pal_menu_t *m, int dir, bool page) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p || dir == 0) return false;
    int step = page ? S_LIST_H - ROW_H : ROW_H + ROW_GAP;
    if (step < 1) step = 1;
    int cont = content_h(p, NULL, S_LIST_W), maxs = cont > S_LIST_H ? cont - S_LIST_H : 0, old = p->spx;
    p->spx += dir < 0 ? -step : step;
    if (p->spx > maxs) p->spx = maxs;
    if (p->spx < 0) p->spx = 0;
    return p->spx != old;
}

/* ---- long text on the selected row: scrolled sideways instead of cut ---- */

#define MARQ_HOLD 4 /* ticks at rest before it starts */
#define MARQ_PX 3   /* logical pixels per tick */
#define MARQ_GAP 12 /* the blank between the end of the text and its repeat */

static int S_MSTEP;           /* ticks since the selection (or the page) changed */
static const void *S_MPAGE;   /* what S_MSTEP counts for */
static int S_MSEL = -2;
static bool S_OVER;           /* the selected row had text that did not fit, in the last draw */

bool pal_menu_tick(pal_menu_t *m) {
    (void)m;
    if (!S_OVER) return false;
    S_MSTEP++;
    return true;
}

/* s at x, y inside w pixels. Normally cut with "..." when it does not fit; on the selected row it scrolls instead. */
static void text_in(kui_canvas_t *k, const kui_font_t *f, int x, int y, int w, const char *s, bool selected) {
    if (w <= 0) return;
    int tw = kui_text_w(f, s);
    if (tw <= w || !selected) {
        char shown[PAL_MENU_VALUE_MAX];
        fit_value(f, s, w, shown, sizeof(shown));
        kui_text(k, f, x, y, shown, true);
        return;
    }
    S_OVER = true;
    int span = tw + MARQ_GAP, off = S_MSTEP > MARQ_HOLD ? ((S_MSTEP - MARQ_HOLD) * MARQ_PX) % span : 0;
    int c0 = k->clip_x0, c1 = k->clip_x1; /* clip to the text's box, then put the list's clip back */
    if (x > k->clip_x0) k->clip_x0 = x;
    if (x + w < k->clip_x1) k->clip_x1 = x + w;
    kui_text(k, f, x - off, y, s, true);
    if (off > 0) kui_text(k, f, x - off + span, y, s, true);
    k->clip_x0 = c0, k->clip_x1 = c1;
}

/* Moves the drawn list's scroll to the selection after a key (the reference scrolls on every focus move, not only when drawing). */
void pal_menu_follow(pal_menu_t *m) {
    int first = 0, count = 0;
    pal_menu_layout(m, NULL, S_LIST_W, S_LIST_H, &first, &count);
}

void pal_menu_draw(pal_menu_t *m, pal_canvas_t *c, pal_rect_t a, const pal_theme_t *t) {
    (void)t;
    kui_canvas_init(&S_KC, c->w, c->h, kui_scale_q());
    kui_canvas_t *k = &S_KC;
    int ay = kui_lg(k, a.y), ah = kui_lg(k, a.y + a.h) - ay;
    kui_clip(k, 0, ay, k->w, ah);
    pal_menu_page_t *p = pal_menu_top(m);
    set_legends(m, p);
    /* the title band and the list below it; the list ends at the view's bottom (the legend bar takes its rows out of the view) */
    int ty = ay + TITLE_TOP;
    bool band = p && p->title; /* the root has no title: the space goes to the list */
    int list_top = band ? ty + TITLE_H + TITLE_GAP : ty;
    int list_h = ay + ah - BOTTOM_PAD - list_top;
    if (list_h < 8) list_h = 8;
    S_LIST_H = list_h;
    S_LIST_W = k->w - 4;
    if (band) kui_fill_round(k, 2, ty, k->w - 4, TITLE_H, 2, true);
    if (band) {
        int tw = kui_text_w(kui_slot(KUI_F_BOLD8), p->title ? p->title : "");
        kui_text(k, kui_slot(KUI_F_BOLD8), (k->w - tw) / 2, ty + 1, p->title ? p->title : "", false);
    }
    if (p && p->n == 0) {
        const char *msg = p->empty ? p->empty : "Nothing here";
        kui_text(k, kui_slot(KUI_F_REG8), LIST_PAD + 5, list_top + list_h / 3, msg, true);
    } else if (p) {
        int first = 0, count = 0;
        if ((const void *)p != S_MPAGE || p->sel != S_MSEL) { /* a new selection starts its text from the beginning */
            S_MPAGE = p, S_MSEL = p->sel, S_MSTEP = 0;
        }
        S_OVER = false;
        static int s_lw, s_lh;
        if (s_lw != k->w || s_lh != list_h) p->follow = true; /* the list changed size (UI scale): show the selection again */
        s_lw = k->w, s_lh = list_h;
        pal_menu_layout(m, t, k->w - 2 * LIST_PAD, list_h, &first, &count);
        bool bar = content_h(p, t, k->w - 2 * LIST_PAD - 3) > list_h;
        int rw = k->w - 2 * LIST_PAD - (bar ? 3 : 0); /* The reference SCROLLBAR_BAR_INSET */
        kui_clip(k, 0, list_top, k->w, list_h); /* a row cut by the list's edges is drawn up to them */
        for (int i = first; i < first + count; i++) {
            int rh = pal_menu_row_h(p, (size_t)i, t, rw);
            if (rh <= 0) continue;
            int yy = list_top + row_top(p, (size_t)i, t, rw) - p->spx;
            draw_row(k, p, (size_t)i, LIST_PAD, yy, rw, i == p->sel);
        }
        kui_clip(k, 0, ay, k->w, ah);
        if (bar) { /* the thumb: every visible row at its own height (a section header is SECTION_BASE), gaps between */
            int content = 0, rows = 0;
            for (size_t i = 0; i < p->n; i++) {
                if (pal_menu_row_hidden(&p->rows[i])) continue;
                content += p->rows[i].kind == PAL_MENU_SECTION ? SECTION_BASE : ROW_H;
                rows++;
            }
            if (rows > 1) content += (rows - 1) * ROW_GAP;
            kui_scrollbar(k, BAR_X, list_top, list_h, p->spx > LIST_PAD ? p->spx - LIST_PAD : 0, list_h - 2 * LIST_PAD,
                          content);
        }
    }
    kui_present(c, k, a);
}
