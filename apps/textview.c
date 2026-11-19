/* The text viewer (view "textview", an overlay Files opens; Esc closes it), drawn as the reference file text viewer
 *: a ListSection with the name and the line count, then the
 * lines in the mono font, one per row, cut at the right edge (the reference does not wrap). The file is read a chunk per pump call, so a
 * big file never holds the desktop. A binary file is a hex dump. Keys: Up/Down scroll, PgUp/PgDn, Home/End, "/" finds (virtual
 * keyboard entry), n and N go to the next and the previous match, L toggles line numbers. The model (rows, search, hex rows) is
 * textview_model.c. The screen is also bound as a document (pal/doc.h), so a session that draws documents (Athena) shows the file in
 * its own look; the classic session draws it with draw below. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "files_model.h"
#include "libapp.h"
#include "pal/desktop.h"
#include "pal/doc.h"
#include "pal/kui.h"
#include "pal/nav.h"
#include "pal/overlay.h"
#include "pal/toast.h"
#include "pal/vk.h"
#include "textview_model.h"
#include "views.h"

#define READ_CHUNK 8192
#define MAX_MARKS 48
#define GUTTER_PAD 6
#define COLS_NOWRAP 1024 /* The reference does not wrap: the rows are whole lines (the drawing cuts them at the screen edge) */
#define BAR_X (k->w - 3) /* the scrollbar's x (logical): the 3 px bar at the right edge, at any UI scale */
#define HEAD_H 16        /* the header: a ListSection in Bold8 (4 + 11 + 1) */

static struct {
    textview_t T;
    nema_obj_t *file;
    char name[NEMA_NAME_MAX];
    uint64_t size;      /* the file's size as stat reported it */
    bool reading;
    int top;            /* first row (text) or line of 8 bytes (hex) on screen */
    int page;            /* rows that fit, from the last draw */
    int nlines;          /* the file's lines: newlines + 1, as the reference counts them */
    bool numbers;
    char query[40];
    bool have_match;
    size_t match;       /* byte offset of the current match */
    int laid_cols;      /* columns the rows were made for; 0 = to be laid out */
    bool owned;
    pal_vk_entry_t ve;
    char vebuf[40];
    kui_canvas_t kc;
} V NEMA_EXT_BSS;

static bool alloc_buffers(void) {
    if (V.owned) return true;
    V.T.data = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, TV_MAX_BYTES);
    V.T.rows = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, TV_MAX_ROWS * sizeof(tv_row_t));
    if (!V.T.data || !V.T.rows) {
        nema_free(V.T.data);
        nema_free(V.T.rows);
        V.T.data = NULL;
        V.T.rows = NULL;
        return false;
    }
    V.owned = true;
    return true;
}

static int digits_of(int n) {
    int d = 1;
    for (n = n > 0 ? n : 1; n >= 10; n /= 10) d++;
    return d;
}

/* The gutter width (line numbers) and the columns that fit a width, in the mono font. */
static int gutter_w(void) { return V.numbers ? digits_of(V.nlines) * FONT_6X8.charW + GUTTER_PAD : 0; }

static void close_file(void) {
    if (V.file) nema_obj_unref(V.file);
    V.file = NULL;
    V.reading = false;
}

static void finish_read(void) {
    close_file();
    V.T.binary = tv_looks_binary(V.T.data, V.T.len);
    V.T.truncated = V.size > TV_MAX_BYTES;
    V.nlines = 1;
    for (size_t i = 0; i < V.T.len; i++)
        if (V.T.data[i] == '\n') V.nlines++;
    V.laid_cols = 0;
    pal_desktop_invalidate();
}

/* One chunk of the file, from the pump. */
static void read_chunk(void) {
    if (!V.reading || !V.file) return;
    size_t room = TV_MAX_BYTES - V.T.len;
    if (room == 0) return finish_read();
    int n = nema_obj_read(V.file, V.T.data + V.T.len, room < READ_CHUNK ? room : READ_CHUNK, -1);
    if (n < 0) {
        psys_toast("Read failed", 2000);
        return finish_read();
    }
    if (n == 0) return finish_read();
    V.T.len += (size_t)n;
    pal_desktop_invalidate();
}

static int max_top(void) {
    int rows = V.T.binary ? tv_hex_rows(&V.T) : V.T.nrows;
    int m = rows - (V.page > 1 ? V.page - 1 : 1);
    return m < 0 ? 0 : m;
}

static void set_top(int row) {
    if (row > max_top()) row = max_top();
    if (row < 0) row = 0;
    V.top = row;
}

static void center_match(size_t off) {
    int row = tv_row_of(&V.T, off);
    set_top(row > 2 ? row - 2 : 0);
}

/* Finds the next (forward) or previous match, wrapping once. */
static void find_match(bool forward) {
    if (V.T.binary || V.T.nrows == 0) { psys_toast("Search works on text", 1500); return; }
    if (!V.query[0]) { psys_toast("No search text", 1500); return; }
    size_t here = V.have_match ? V.match : V.T.rows[V.top].start;
    long at = -1;
    if (forward) {
        at = tv_find(&V.T, V.have_match ? here + 1 : here, V.query, true);
        if (at < 0 && here > 0) {
            at = tv_find(&V.T, 0, V.query, true);
            if (at >= 0) psys_toast("Search wrapped to the top", 1200);
        }
    } else {
        long last = -1;
        for (long f = tv_find(&V.T, 0, V.query, true); f >= 0 && (size_t)f < here; f = tv_find(&V.T, (size_t)f + 1, V.query, true))
            last = f;
        if (last < 0) {
            for (long f = tv_find(&V.T, 0, V.query, true); f >= 0; f = tv_find(&V.T, (size_t)f + 1, V.query, true)) last = f;
            if (last >= 0) psys_toast("Search wrapped to the bottom", 1200);
        }
        at = last;
    }
    if (at < 0) { psys_toast("Not found", 1500); return; }
    V.have_match = true;
    V.match = (size_t)at;
    center_match(V.match);
    pal_desktop_invalidate();
}

static void on_find(int len, void *ctx) {
    (void)ctx;
    if (len < 0) return;
    V.have_match = false;
    find_match(true);
}

static void open_find(void) {
    if (V.T.binary) { psys_toast("Search works on text", 1500); return; }
    memset(&V.ve, 0, sizeof(V.ve));
    snprintf(V.vebuf, sizeof(V.vebuf), "%s", V.query);
    V.ve.title = "Find";
    V.ve.initial = V.vebuf;
    V.ve.buf = V.vebuf;
    V.ve.cap = sizeof(V.vebuf);
    V.ve.allow_empty = false;
    if (pal_vk_edit_async(&V.ve, on_find, NULL) != NEMA_OK) psys_toast("Keyboard is busy", 1500);
    else snprintf(V.query, sizeof(V.query), "%s", V.vebuf);
}

/* ---- drawing (the reference text viewer: the header, then the lines) ---- */
static kui_canvas_t *canvas(pal_canvas_t *c) {
    kui_canvas_init(&V.kc, c->w, c->h, kui_scale_q());
    return &V.kc;
}

/* One row of text at (x, y): tabs go to the next multiple of four columns, a match is drawn inverted. */
static void draw_text_row(kui_canvas_t *k, int x, int y, int row, const uint32_t *marks, int nmarks, int cols_limit) {
    const tv_row_t *r = &V.T.rows[row];
    const int cw = FONT_6X8.charW, ch = FONT_6X8.charH;
    int col = 0, qlen = (int)strlen(V.query);
    size_t p = r->start;
    while (p < r->end && col < cols_limit) {
        uint32_t cp;
        size_t n = tv_decode(V.T.data + p, r->end - p, &cp);
        bool hit = false;
        for (int i = 0; i < nmarks && !hit; i++) hit = p >= marks[i] && p < marks[i] + (size_t)qlen;
        if (cp == '\r') {
            /* carriage returns are not drawn */
        } else if (cp == '\t') {
            int w = 4 - col % 4;
            if (hit) kui_fill(k, x + col * cw, y, w * cw, ch, true);
            col += w;
        } else {
            char g = cp >= 0x20 && cp < 0x7F ? (char)cp : '?';
            if (hit) {
                kui_fill(k, x + col * cw, y, cw, ch, true);
                kui_char(k, &FONT_6X8, x + col * cw, y, g, false);
            } else {
                kui_char(k, &FONT_6X8, x + col * cw, y, g, true);
            }
            col++;
        }
        p += n;
    }
}

static void draw_hex_row(kui_canvas_t *k, int x, int y, int row) {
    size_t base = (size_t)row * TV_HEX_BYTES;
    char line[40];
    int n = snprintf(line, sizeof(line), "%06lX", (unsigned long)base);
    for (int i = 0; i < TV_HEX_BYTES && base + (size_t)i < V.T.len; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %02X", V.T.data[base + (size_t)i]);
    kui_text(k, &FONT_6X8, x, y, line, true);
}

static void draw_text_body(kui_canvas_t *k, int y, int bottom, int gw) {
    int cols_limit = (k->w - gw) / FONT_6X8.charW;
    if (cols_limit < 1) cols_limit = 1;
    if (V.laid_cols == 0) {
        tv_layout(&V.T, COLS_NOWRAP);
        V.laid_cols = COLS_NOWRAP;
        if (V.top >= V.T.nrows) V.top = V.T.nrows > 0 ? V.T.nrows - 1 : 0;
    }
    int rh = FONT_6X8.charH;
    int vis = (bottom - y) / rh;
    if (vis < 1) vis = 1;
    V.page = vis;
    set_top(V.top);
    int last = V.top + vis - 1;
    if (last >= V.T.nrows) last = V.T.nrows - 1;
    uint32_t marks[MAX_MARKS];
    int nm = V.query[0] && V.T.nrows > 0 ? tv_matches(&V.T, V.top, last, V.query, true, marks, MAX_MARKS) : 0;
    for (int i = 0; i < vis && V.top + i < V.T.nrows; i++) {
        int row = V.top + i, ry = y + i * rh;
        if (V.numbers && V.T.rows[row].line) {
            char num[12];
            snprintf(num, sizeof(num), "%lu", (unsigned long)V.T.rows[row].line);
            kui_text(k, &FONT_6X8, gw - (int)strlen(num) * FONT_6X8.charW - 2, ry, num, true);
        }
        draw_text_row(k, gw, ry, row, marks, nm, cols_limit);
    }
    if (V.T.nrows > vis) kui_scrollbar(k, BAR_X, y, vis * rh, V.top, vis, V.T.nrows);
}

static void draw_hex_body(kui_canvas_t *k, int y, int bottom) {
    int rh = FONT_6X8.charH;
    int vis = (bottom - y) / rh;
    if (vis < 1) vis = 1;
    V.page = vis;
    int rows = tv_hex_rows(&V.T);
    set_top(V.top);
    for (int i = 0; i < vis && V.top + i < rows; i++) draw_hex_row(k, 0, y + i * rh, V.top + i);
    if (rows > vis) kui_scrollbar(k, BAR_X, y, vis * rh, V.top, vis, rows);
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_t *k = canvas(c);
    int ay = kui_lg(k, a.y), ah = kui_lg(k, a.y + a.h) - ay;
    kui_clip(k, 0, ay, k->w, ah);
    char title[64];
    if (V.T.binary) snprintf(title, sizeof(title), "%s (hex)", V.name);
    else if (V.reading) snprintf(title, sizeof(title), "%s", V.name);
    else if (V.T.truncated) snprintf(title, sizeof(title), "%s (%d+ lines)", V.name, V.nlines);
    else snprintf(title, sizeof(title), "%s (%d lines)", V.name, V.nlines);
    int y = ay + kui_section(k, 0, ay, &FONT_BOLD8, title);
    int bottom = ay + ah;
    if (V.reading) {
        char msg[40];
        snprintf(msg, sizeof(msg), "Reading... %luK", (unsigned long)(V.T.len / 1024));
        kui_text(k, &FONT_REG8, 5, y + 2, msg, true);
    } else if (V.T.len == 0) {
        kui_text(k, &FONT_REG8, 5, y + 2, "(empty)", true);
    } else if (V.T.binary) {
        draw_hex_body(k, y, bottom);
    } else {
        draw_text_body(k, y, bottom, gutter_w());
    }
    kui_present(c, k, a);
}

/* ---- the document (pal/doc.h) ---- */
static void ensure_layout(void) {
    if (V.laid_cols == 0 && !V.reading && !V.T.binary && V.T.len) {
        tv_layout(&V.T, COLS_NOWRAP);
        V.laid_cols = COLS_NOWRAP;
    }
}
static void doc_title(void *ctx, char *out, size_t cap) {
    (void)ctx;
    if (V.T.binary) snprintf(out, cap, "%s (hex)", V.name);
    else if (V.reading) snprintf(out, cap, "%s", V.name);
    else if (V.T.truncated) snprintf(out, cap, "%s (%d+ lines)", V.name, V.nlines);
    else snprintf(out, cap, "%s (%d lines)", V.name, V.nlines);
}
static int doc_count(void *ctx) {
    (void)ctx;
    if (V.reading) return 0;
    ensure_layout();
    return V.T.binary ? tv_hex_rows(&V.T) : V.T.nrows;
}
static void doc_line(void *ctx, int row, char *out, size_t cap, bool *mark) {
    (void)ctx;
    out[0] = 0;
    if (V.T.binary) {
        size_t base = (size_t)row * TV_HEX_BYTES;
        int n = snprintf(out, cap, "%06lX", (unsigned long)base);
        for (int i = 0; i < TV_HEX_BYTES && base + (size_t)i < V.T.len && (size_t)n < cap; i++)
            n += snprintf(out + n, cap - (size_t)n, " %02X", V.T.data[base + (size_t)i]);
        return;
    }
    if (row < 0 || row >= V.T.nrows) return;
    const tv_row_t *r = &V.T.rows[row];
    size_t o = 0;
    if (V.numbers) o = (size_t)snprintf(out, cap, r->line ? "%4lu  " : "      ", (unsigned long)r->line);
    for (size_t p = r->start; p < r->end && o + 5 < cap;) { /* UTF-8 as it is; tabs to the next multiple of four */
        uint32_t cp;
        size_t n = tv_decode(V.T.data + p, r->end - p, &cp);
        if (cp == '\t') {
            do out[o++] = ' ';
            while (o % 4 && o + 1 < cap);
        } else if (cp >= 0x20 && cp != 0x7F) {
            memcpy(out + o, V.T.data + p, n);
            o += n;
        }
        p += n;
    }
    out[o] = 0;
    uint32_t hit;
    if (V.query[0]) *mark = tv_matches(&V.T, row, row, V.query, true, &hit, 1) > 0;
}
static void doc_empty(void *ctx, char *out, size_t cap) {
    (void)ctx;
    if (V.reading) snprintf(out, cap, "Reading... %luK", (unsigned long)(V.T.len / 1024));
    else snprintf(out, cap, "(empty)");
}
static void doc_relayout(void *ctx) {
    (void)ctx;
    set_top(V.top); /* inside the rows with the new page */
}
static pal_doc_t DOC = {.relayout = doc_relayout, .title = doc_title, .count = doc_count, .line = doc_line, .empty = doc_empty, .top = &V.top, .vis = &V.page,
                        .mono = true};

/* ---- keys, pump, lifecycle ---- */
static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    int rows = V.T.binary ? tv_hex_rows(&V.T) : V.T.nrows;
    int page = V.page > 1 ? V.page - 1 : 1;
    switch (k->sym) {
    case PAL_KEY_UP: set_top(V.top - 1); break;
    case PAL_KEY_DOWN: set_top(V.top + 1); break;
    case PAL_KEY_PGUP: set_top(V.top - page); break;
    case PAL_KEY_PGDN: set_top(V.top + page); break;
    case PAL_KEY_HOME: V.top = 0; break;
    case PAL_KEY_END: set_top(rows); break;
    case PAL_KEY_ESC: return false; /* the overlay closes */
    case PAL_KEY_CHAR:
        if (k->cp == '/') open_find();
        else if (k->cp == 'n') find_match(true);
        else if (k->cp == 'N') find_match(false);
        else if (k->cp == 'l' || k->cp == 'L') {
            V.numbers = !V.numbers;
            V.laid_cols = 0;
        } else return true;
        break;
    default: return true;
    }
    DOC.gen++;
    pal_desktop_invalidate();
    return true;
}

/* Up to eight chunks (64 KB) per call: a full 256 KB file loads in a few loops, and the keys still get a turn between them. */
static void pump(pal_view_t *v) {
    (void)v;
    for (int i = 0; i < 8 && V.reading; i++) read_chunk();
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    if (V.reading) pal_desktop_invalidate();
}

static void leave(pal_view_t *v) {
    (void)v;
    close_file();
}

void pal_textview_open(const char *path) {
    if (!alloc_buffers()) { psys_toast("Out of memory", 1500); return; }
    close_file();
    nema_stat_t st;
    if (nema_fs_stat(path, &st) != NEMA_OK || st.type != NEMA_T_FILE) { psys_toast("Cannot open file", 1500); return; }
    if (nema_fs_open(path, NEMA_O_READ, &V.file) != NEMA_OK) { psys_toast("Cannot open file", 1500); return; }
    snprintf(V.name, sizeof(V.name), "%s", files_base(path));
    V.size = st.size;
    V.T.len = 0;
    V.T.truncated = V.T.binary = false;
    V.T.nrows = 0;
    V.T.lines = 0;
    V.nlines = 0;
    V.top = 0;
    V.have_match = false;
    V.laid_cols = 0;
    V.reading = true;
    if (!V.page) V.page = 12; /* until a look has drawn the lines */
    pal_doc_bind("textview", &DOC);
    if (pal_desktop_overlay_push(&pal_textview_view) != NEMA_OK) {
        close_file();
        psys_toast("Viewer is busy", 1500);
    }
}

pal_view_t pal_textview_view = {
    .id = "textview",
    .title = "Text",
    .flags = PAL_VIEW_OPAQUE | PAL_VIEW_HIDDEN,
    .draw = draw,
    .key = key,
    .pump = pump,
    .tick = tick,
    .tick_ms = 100,
    .leave = leave,
};
