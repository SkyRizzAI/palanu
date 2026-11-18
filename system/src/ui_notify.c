/* The notification center: opaque. A header with the count, the list newest first (app icon, bold title, one line of
 * text, age, and an unread dot), and a detail page per item (the full text wrapped, buttons Dismiss and Close). The focused
 * row's text scrolls when it does not fit. TAB clears everything after a second TAB. Leaving the center marks the list read.
 * Nothing here draws a toast: that is the overlay layer's job. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/dialog.h"
#include "pal/icons.h"
#include "pal/menu.h"
#include "pal/kui.h"
#include "pal/notify.h"
#include "pal/registry.h"
#include "pal/time.h"
#include "pal/ui.h"

#define MAX_LINES 6
#define MARQ_EVERY 2   /* ticks per marquee step */

static struct {
    psys_notify_t items[PSYS_NOTIFY_RING];
    int n;
    int focus;     /* list row, or the button in the detail page */
    bool detail;   /* the detail page of items[focus_item] */
    int item;      /* the detail page's item */
    int marq, marq_tick;
} N NEMA_EXT_BSS;

static void snapshot(void) { N.n = psys_notify_list(N.items, PSYS_NOTIFY_RING); }

static void on_clear(bool ok, void *ctx) {
    (void)ctx;
    if (!ok) return;
    psys_notify_clear();
    N.focus = 0;
    N.marq = 0;
}

/* ---- drawing: kui, at the UI scale, in the layout of the Settings pages (menu_draw.c): an inverted title band, rows with an
 * 8 px icon, the selection inverted. Logical pixels throughout. ---- */

#define TITLE_H 13  /* the band: BOLD8 cell + 2, as the Settings title */
#define TITLE_GAP 3
#define PAD 2       /* the list's inset */
#define ICON_X 4
#define TEXT_X 14   /* the title and text column: the icon (8) and its gap */
#define BAR_X (k->w - 3)

static kui_canvas_t K NEMA_EXT_BSS;

static const kui_font_t *f_title(void) { return kui_slot(KUI_F_BOLD8); }
static const kui_font_t *f_text(void) { return kui_slot(KUI_F_REG8); }
static int row_h(void) { return f_title()->charH + f_text()->charH + 4; }

/* The longest prefix of s (with "..." when cut) that fits in w. */
static const char *fit(const kui_font_t *f, const char *s, int w, char *out, size_t cap) {
    if (kui_text_w(f, s) <= w) return s;
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    while (n > 0) {
        snprintf(out, cap, "%.*s...", (int)n, s);
        if (kui_text_w(f, out) <= w) return out;
        n--;
    }
    return "";
}

/* Greedy word wrap of text into lines of at most width px. Returns the number of lines (at most MAX_LINES). */
static int wrap(const kui_font_t *f, const char *text, int width, char lines[MAX_LINES][96]) {
    int n = 0;
    char cur[96] = "";
    const char *p = text;
    while (*p && n < MAX_LINES) {
        const char *e = p;
        while (*e && *e != ' ') e++;
        char word[96];
        size_t wl = (size_t)(e - p) < sizeof(word) - 1 ? (size_t)(e - p) : sizeof(word) - 1;
        memcpy(word, p, wl);
        word[wl] = 0;
        char trial[96];
        snprintf(trial, sizeof(trial), cur[0] ? "%s %s" : "%s", cur[0] ? cur : word, word);
        if (cur[0] && kui_text_w(f, trial) > width) {
            snprintf(lines[n++], 96, "%s", cur);
            snprintf(cur, sizeof(cur), "%s", word);
        } else {
            snprintf(cur, sizeof(cur), "%s", trial);
        }
        p = *e ? e + 1 : e;
    }
    if (cur[0] && n < MAX_LINES) snprintf(lines[n++], 96, "%s", cur);
    return n;
}

static void band(kui_canvas_t *k, int y, const char *title) {
    kui_fill_round(k, PAD, y, k->w - 2 * PAD, TITLE_H, 2, true);
    kui_text(k, f_title(), (k->w - kui_text_w(f_title(), title)) / 2, y + 1, title, false);
}

static void draw_row(kui_canvas_t *k, int x, int y, int w, const psys_notify_t *n, bool focus, int marq) {
    const int h = row_h();
    static pal_reg_app_t entry;
    const char *icon = psys_reg_find(n->source, &entry) ? (entry.icon[0] ? entry.icon : entry.id) : "notifications";
    pal_menu_icon8(k, icon, x + ICON_X, y + (h - 8) / 2);
    char age[12], buf[96];
    pal_ui_fmt_age(pal_time_epoch(), n->time, pal_time_tz_minutes(), age, sizeof(age));
    const kui_font_t *tf = f_title(), *xf = f_text();
    int aw = kui_text_w(xf, age), right = x + w - 3;
    kui_text(k, xf, right - aw, y + 2, age, true);
    if (!n->read) kui_fill(k, right - aw - 5, y + 4, 2, 2, true); /* the unread dot */
    int tx = x + TEXT_X, tw = right - aw - 7 - tx;
    kui_text(k, tf, tx, y + 1, fit(tf, n->title[0] ? n->title : n->source, tw, buf, sizeof(buf)), true);
    int ly = y + tf->charH + 2, lw = right - tx, full = kui_text_w(xf, n->text);
    if (focus && full > lw) { /* the focused row's text scrolls when it does not fit */
        int span = full + 16, off = marq % span;
        kui_clip(k, tx, ly, lw, xf->charH);
        kui_text(k, xf, tx - off, ly, n->text, true);
        kui_text(k, xf, tx - off + span, ly, n->text, true);
        kui_clip_reset(k);
    } else {
        kui_text(k, xf, tx, ly, fit(xf, n->text, lw, buf, sizeof(buf)), true);
    }
    kui_row_frame(k, x, y, w, h, focus); /* the selection, inverted as in Settings */
}

static const pal_legend_item_t LIST_LEGENDS[] = {
    {PAL_ACT_MOVE_V, "move", NULL}, {PAL_ACT_OK, "open", NULL}, {PAL_ACT_TAB, "clear all", NULL}, {PAL_ACT_BACK, "back", NULL}};
static const pal_legend_item_t EMPTY_LEGENDS[] = {{PAL_ACT_BACK, "back", NULL}}; /* nothing to move to or open */
static const pal_legend_item_t DETAIL_LEGENDS[] = {
    {PAL_ACT_MOVE_H, "choose", NULL}, {PAL_ACT_OK, "press", NULL}, {PAL_ACT_BACK, "back", NULL}};

static void draw_list(kui_canvas_t *k, int ay, int ah) {
    if (N.n == 0) pal_legend_set(EMPTY_LEGENDS, 1);
    else pal_legend_set(LIST_LEGENDS, 4);
    char title[32];
    snprintf(title, sizeof(title), "Notifications (%d)", N.n);
    int ty = ay + 1;
    band(k, ty, title);
    int top = ty + TITLE_H + TITLE_GAP, h = ay + ah - 1 - top;
    if (N.n == 0) {
        const pal_icon_t *ic = pal_icon_find("notifications", 16);
        const char *msg = "No notifications";
        int cy = top + (h - 16 - 4 - f_text()->charH) / 2;
        if (ic) kui_bitmap(k, (k->w - 16) / 2, cy, 16, 16, ic->bits, true);
        kui_text(k, f_text(), (k->w - kui_text_w(f_text(), msg)) / 2, cy + 20, msg, true);
        return;
    }
    int rh = row_h() + 1, vis = h / rh;
    if (vis < 1) vis = 1;
    int first = pal_ui_window_first(N.n, N.focus, vis);
    bool bar = N.n > vis;
    int w = k->w - 2 * PAD - (bar ? 4 : 0);
    for (int i = first; i < N.n && i < first + vis; i++)
        draw_row(k, PAD, top + (i - first) * rh, w, &N.items[i], i == N.focus, i == N.focus ? N.marq : 0);
    if (bar) kui_scrollbar(k, BAR_X, top, vis * rh, first, vis, N.n);
}

static void draw_detail(kui_canvas_t *k, int ay, int ah) {
    pal_legend_set(DETAIL_LEGENDS, 3);
    if (N.item < 0 || N.item >= N.n) return;
    const psys_notify_t *n = &N.items[N.item];
    int ty = ay + 1;
    band(k, ty, "Notification");
    const kui_font_t *tf = f_title(), *xf = f_text();
    int x = PAD + 3, w = k->w - 2 * x, y = ty + TITLE_H + TITLE_GAP + 1;
    char age[12], line[96], buf[96];
    pal_ui_fmt_age(pal_time_epoch(), n->time, pal_time_tz_minutes(), age, sizeof(age));
    snprintf(line, sizeof(line), "%s  %s", n->source, age);
    kui_text(k, xf, x, y, fit(xf, line, w, buf, sizeof(buf)), true);
    y += xf->charH + 2;
    kui_text(k, tf, x, y, fit(tf, n->title[0] ? n->title : n->source, w, buf, sizeof(buf)), true);
    y += tf->charH + 3;
    /* the two buttons at the bottom of the area (the legend bar is below it); the text uses the rows above them */
    int bh = tf->charH + 3, by = ay + ah - bh - 2;
    char lines[MAX_LINES][96];
    int nl = wrap(xf, n->text, w, lines);
    for (int i = 0; i < nl && y + xf->charH <= by - 2; i++, y += xf->charH + 1) kui_text(k, xf, x, y, lines[i], true);
    const char *labels[2] = {"Dismiss", "Close"};
    int bx = x;
    for (int i = 0; i < 2; i++) {
        int bw = kui_text_w(tf, labels[i]) + 10;
        bool sel = N.focus == i;
        if (sel) kui_fill_round(k, bx, by, bw, bh, 2, true);
        else kui_round(k, bx, by, bw, bh, 2, true);
        kui_text(k, tf, bx + 5, by + 2, labels[i], !sel);
        bx += bw + 6;
    }
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    snapshot();
    kui_canvas_init(&K, c->w, c->h, kui_scale_q());
    kui_canvas_t *k = &K;
    int ay = kui_lg(k, a.y), ah = kui_lg(k, a.y + a.h) - ay;
    kui_clip(k, 0, ay, k->w, ah);
    kui_clear(k, false);
    if (N.detail) draw_detail(k, ay, ah);
    else draw_list(k, ay, ah);
    kui_present(c, k, a);
}

static void enter(pal_view_t *v) {
    (void)v;
    N.focus = 0;
    N.detail = false;
    N.marq = 0;
    N.marq_tick = 0;
    snapshot();
}

static void leave(pal_view_t *v) {
    (void)v;
    psys_notify_mark_all_read();
}

/* The marquee: while the focused row's text does not fit, step it every MARQ_EVERY ticks and repaint the list. */
static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    if (N.detail || N.n == 0 || N.focus >= N.n) return;
    if (++N.marq_tick < MARQ_EVERY) return;
    N.marq_tick = 0;
    if (kui_text_w(f_text(), N.items[N.focus].text) <= K.w - TEXT_X - 2 * PAD - 3) return; /* fits: nothing scrolls */
    N.marq++;
    pal_desktop_invalidate();
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    snapshot();
    if (N.detail) {
        switch (k->sym) {
        case PAL_KEY_LEFT:
        case PAL_KEY_RIGHT: N.focus = N.focus ? 0 : 1; return true;
        case PAL_KEY_ESC: N.detail = false; N.focus = N.item; return true;
        case PAL_KEY_ENTER:
            if (N.focus == 0 && N.item < N.n) (void)psys_notify_cancel(N.items[N.item].source, (int32_t)N.items[N.item].id);
            N.detail = false;
            N.focus = N.item < N.n ? N.item : 0;
            N.marq = 0;
            return true;
        default: return false;
        }
    }
    switch (k->sym) {
    case PAL_KEY_UP:
        if (N.focus > 0) N.focus--;
        N.marq = 0;
        return true;
    case PAL_KEY_DOWN:
        if (N.focus + 1 < N.n) N.focus++;
        N.marq = 0;
        return true;
    case PAL_KEY_ENTER:
        if (N.n == 0) return true;
        N.detail = true;
        N.item = N.focus;
        N.focus = 1; /* Close is the default button: a stray Enter does not dismiss */
        return true;
    case PAL_KEY_TAB:
        if (N.n > 0) (void)pal_dialog_confirm("Clear all", "Remove all notifications?", "Clear", "Cancel", on_clear, NULL);
        return true;
    case PAL_KEY_ESC:
        pal_desktop_home();
        return true;
    default:
        return false;
    }
}

pal_view_t pal_notify_view = {
    .id = "notifications", .title = "Notifications", .flags = PAL_VIEW_FULLSCREEN, .enter = enter, .leave = leave,
    .draw = draw, .key = key, .tick = tick, .tick_ms = 200, .legends = LIST_LEGENDS, .nlegends = 4,
};
