/* The text editor (view "editor"): a full-screen editor for text files. `edit [FILE]` opens it from the shell, Files opens it with
 * Edit, and the launcher opens it on a new text. The text is a gap buffer (editor_buf.c) in PSRAM, up to EDIT_MAX bytes.
 *
 * Keys: the arrows move (by character, UTF-8 aware), Home and End go to the line's ends, PgUp and PgDn by a screen, Enter starts a
 * line with the indent of the one above, Tab inserts four spaces, Backspace and Delete erase. Ctrl+S saves (a new text asks for
 * its path first), Ctrl+F finds (asks the text; Ctrl+N goes to the next match), Ctrl+G goes to a line. Esc closes; with unsaved
 * changes it asks Save, Discard or Keep editing.
 *
 * Drawn as a terminal is: the Spleen 6x12 font in the terminal's colours, which are the running desktop session's (ADR-0012), so
 * the editor takes the session's look without asking which one runs. A line longer than the screen scrolls sideways. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "editor_buf.h"
#include "files_model.h"
#include "libapp.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/term.h"
#include "pal/theme.h"
#include "pal/toast.h"
#include "pal/vk.h"
#include "views.h"

#define EDIT_MAX (256u * 1024u) /* the largest file it edits */
#define EDIT_CAP (EDIT_MAX + 64u * 1024u)
#define LOAD_CHUNK 16384
#define C_GUTTER 8              /* xterm's bright black: the line numbers */

static struct {
    char *mem;
    ebuf_t b;
    char path[NEMA_PATH_MAX];  /* "" for a new text */
    char want[NEMA_PATH_MAX];  /* pal_editor_open: the path the pump opens */
    bool want_set, loading, modified, ready, closing;
    nema_obj_t *in;            /* the file being read */
    size_t top, left;          /* the first line and the first column on screen */
    size_t goal;               /* the column Up and Down aim for */
    int rows, cols;
    char query[40];
    pal_vk_entry_t ve;
    char vebuf[NEMA_PATH_MAX];
    int ask;                   /* what the keyboard is asking: 1 save as, 2 find, 3 go to line */
} E NEMA_EXT_BSS;

static const pal_font_t *font(void) { return &PAL_FONT_SPLEEN_6X12; }
static bool cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; } /* a UTF-8 continuation byte */

/* The column of pos on its line, in characters. */
static size_t col_of(size_t pos) {
    size_t s = eb_line_start(&E.b, pos), n = 0;
    for (size_t p = s; p < pos; p++) n += !cont(eb_at(&E.b, p));
    return n;
}

/* The position at column col of the line that starts at s (its end when the line is shorter). */
static size_t pos_at_col(size_t s, size_t col) {
    size_t end = eb_line_end(&E.b, s), p = s;
    while (p < end && col > 0) {
        p++;
        while (p < end && cont(eb_at(&E.b, p))) p++;
        col--;
    }
    return p;
}

static void go(size_t pos, bool keep_goal) {
    eb_move(&E.b, pos);
    if (!keep_goal) E.goal = col_of(eb_cursor(&E.b));
    pal_desktop_invalidate();
}

/* ---- files ---- */

static bool alloc_buf(void) {
    if (E.mem) return true;
    E.mem = nema_alloc(NEMA_MEM_EXTERNAL, NEMA_OWNER_KERNEL, EDIT_CAP);
    return E.mem != NULL;
}

static void reset(const char *path) {
    eb_init(&E.b, E.mem, EDIT_CAP);
    nema_strlcpy(E.path, path ? path : "", sizeof(E.path));
    E.top = E.left = E.goal = 0;
    E.modified = E.loading = false;
    if (E.in) nema_obj_unref(E.in);
    E.in = NULL;
    if (path && path[0] && nema_fs_open(path, NEMA_O_READ, &E.in) == NEMA_OK) E.loading = true; /* a missing file is a new one */
}

static void load_step(void) {
    char chunk[512];
    for (size_t done = 0; E.loading && done < LOAD_CHUNK; done += sizeof(chunk)) {
        int n = nema_obj_read(E.in, chunk, sizeof(chunk), -1); /* a file never waits long; 0 would only say "again" */
        if (n > 0 && eb_len(&E.b) + (size_t)n > EDIT_MAX) n = NEMA_ERR_RANGE;
        if (n > 0) {
            eb_insert(&E.b, chunk, (size_t)n);
            continue;
        }
        if (n < 0) psys_toast(n == NEMA_ERR_RANGE ? "File too big: the first 256 KB is shown" : "Cannot read the file", 2000);
        E.loading = false;
        nema_obj_unref(E.in);
        E.in = NULL;
        eb_move(&E.b, 0);
    }
    pal_desktop_invalidate();
}

static bool save_to(const char *path) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) {
        psys_toast("Cannot write there", 1500);
        return false;
    }
    char chunk[512];
    size_t len = eb_len(&E.b);
    bool ok = true;
    for (size_t p = 0; p < len && ok; p += sizeof(chunk)) {
        size_t n = eb_copy(&E.b, p, chunk, sizeof(chunk));
        ok = nema_obj_write(f, chunk, n, 2000) == (int)n;
    }
    nema_obj_unref(f);
    if (!ok) {
        psys_toast("Write failed: the file may be cut short", 2000);
        return false;
    }
    nema_strlcpy(E.path, path, sizeof(E.path));
    E.modified = false;
    psys_toast("Saved", 1000);
    pal_desktop_invalidate();
    return true;
}

/* ---- questions: save as, find, go to line ---- */

static void find_next(void) {
    if (!E.query[0]) return;
    long at = eb_find(&E.b, eb_cursor(&E.b) + 1, E.query);
    if (at < 0) psys_toast("Not found", 1000);
    else go((size_t)at, false);
}

static void close_now(void);

static void on_answer(int len, void *ctx) {
    (void)ctx;
    int what = E.ask;
    E.ask = 0;
    if (len < 0) {
        if (what == 1) E.closing = false;
        return;
    }
    if (what == 1) {
        bool close_after = E.closing;
        if (save_to(E.vebuf) && close_after) close_now();
        E.closing = false;
    } else if (what == 2) {
        nema_strlcpy(E.query, E.vebuf, sizeof(E.query));
        find_next();
    } else if (what == 3) {
        long line = strtol(E.vebuf, NULL, 10);
        if (line >= 1) go(eb_line_pos(&E.b, (size_t)line - 1), false);
    }
}

static void ask(int what, const char *title, const char *initial) {
    E.ask = what;
    nema_strlcpy(E.vebuf, initial ? initial : "", sizeof(E.vebuf));
    memset(&E.ve, 0, sizeof(E.ve));
    E.ve.title = title;
    E.ve.initial = E.vebuf;
    E.ve.buf = E.vebuf;
    E.ve.cap = what == 2 ? sizeof(E.query) : sizeof(E.vebuf);
    if (pal_vk_edit_async(&E.ve, on_answer, NULL) != NEMA_OK) E.ask = 0;
}

static void save(void) {
    if (E.path[0]) {
        if (save_to(E.path) && E.closing) close_now();
        E.closing = false;
        return;
    }
    ask(1, "Save as", "/home/untitled.txt");
}

/* ---- closing ---- */

static void close_now(void) {
    E.ready = false;
    E.modified = false;
    nema_free(E.mem); /* 320 KB of PSRAM back while the editor is not in use */
    E.mem = NULL;
    pal_app_exit();
}

static const char *const CLOSE_ITEMS[] = {"Save and close", "Discard changes", "Keep editing"};
static void on_close_pick(int i, void *ctx) {
    (void)ctx;
    if (i == 0) {
        E.closing = true;
        save();
    } else if (i == 1) {
        close_now();
    }
}

/* ---- editing ---- */

static void insert(const char *s, size_t n) {
    if (E.loading) return;
    if (eb_len(&E.b) + n > EDIT_MAX || !eb_insert(&E.b, s, n)) {
        psys_toast("The text is full (256 KB)", 1500);
        return;
    }
    E.modified = true;
    E.goal = col_of(eb_cursor(&E.b));
    pal_desktop_invalidate();
}

static void newline(void) { /* the new line starts with the indent of this one */
    size_t s = eb_line_start(&E.b, eb_cursor(&E.b));
    char ind[33] = "\n";
    size_t n = 1;
    while (n < sizeof(ind) - 1 && (eb_at(&E.b, s + n - 1) == ' ' || eb_at(&E.b, s + n - 1) == '\t') && s + n - 1 < eb_cursor(&E.b))
        ind[n] = eb_at(&E.b, s + n - 1), n++;
    insert(ind, n);
}

static void back(void) {
    size_t c = eb_cursor(&E.b), n = 0;
    if (c == 0) return;
    do n++;
    while (c > n && cont(eb_at(&E.b, c - n)));
    eb_delete_back(&E.b, n);
    E.modified = true;
    E.goal = col_of(eb_cursor(&E.b));
    pal_desktop_invalidate();
}

static void del(void) {
    size_t c = eb_cursor(&E.b), len = eb_len(&E.b), n = 0;
    if (c >= len) return;
    do n++;
    while (c + n < len && cont(eb_at(&E.b, c + n)));
    eb_delete_fwd(&E.b, n);
    E.modified = true;
    pal_desktop_invalidate();
}

static void left(void) {
    size_t c = eb_cursor(&E.b);
    if (c == 0) return;
    c--;
    while (c > 0 && cont(eb_at(&E.b, c))) c--;
    go(c, false);
}

static void right(void) {
    size_t c = eb_cursor(&E.b), len = eb_len(&E.b);
    if (c >= len) return;
    c++;
    while (c < len && cont(eb_at(&E.b, c))) c++;
    go(c, false);
}

static void vertical(int lines) {
    size_t c = eb_cursor(&E.b), s = eb_line_start(&E.b, c);
    for (; lines < 0 && s > 0; lines++) s = eb_line_start(&E.b, s - 1);
    for (; lines > 0; lines--) {
        size_t e = eb_line_end(&E.b, s);
        if (e >= eb_len(&E.b)) break;
        s = e + 1;
    }
    go(pos_at_col(s, E.goal), true);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (!E.ready) return false;
    if ((k->mods & PAL_MOD_CTRL) && k->sym == PAL_KEY_CHAR) {
        switch (k->cp | 32) {
        case 's': save(); return true;
        case 'f': ask(2, "Find", E.query); return true;
        case 'n': find_next(); return true;
        case 'g': ask(3, "Go to line", ""); return true;
        default: return true;
        }
    }
    switch (k->sym) {
    case PAL_KEY_ESC:
        if (!E.modified) {
            close_now();
            return true;
        }
        (void)pal_picker_open("Unsaved changes", CLOSE_ITEMS, 3, on_close_pick, NULL);
        return true;
    case PAL_KEY_LEFT: left(); return true;
    case PAL_KEY_RIGHT: right(); return true;
    case PAL_KEY_UP: vertical(-1); return true;
    case PAL_KEY_DOWN: vertical(1); return true;
    case PAL_KEY_PGUP: vertical(-(E.rows > 1 ? E.rows - 1 : 1)); return true;
    case PAL_KEY_PGDN: vertical(E.rows > 1 ? E.rows - 1 : 1); return true;
    case PAL_KEY_HOME: go(eb_line_start(&E.b, eb_cursor(&E.b)), false); return true;
    case PAL_KEY_END: go(eb_line_end(&E.b, eb_cursor(&E.b)), false); return true;
    case PAL_KEY_ENTER: newline(); return true;
    case PAL_KEY_TAB: insert("    ", 4); return true;
    case PAL_KEY_BACKSPACE: back(); return true;
    case PAL_KEY_DELETE: del(); return true;
    case PAL_KEY_CHAR: {
        char u[4];
        uint32_t cp = k->cp;
        size_t n;
        if (cp < 0x20 || cp == 0x7F) return true;
        if (cp < 0x80) u[0] = (char)cp, n = 1;
        else if (cp < 0x800) u[0] = (char)(0xC0 | cp >> 6), u[1] = (char)(0x80 | (cp & 0x3F)), n = 2;
        else if (cp < 0x10000) u[0] = (char)(0xE0 | cp >> 12), u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)), u[2] = (char)(0x80 | (cp & 0x3F)), n = 3;
        else u[0] = (char)(0xF0 | cp >> 18), u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)), u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)),
             u[3] = (char)(0x80 | (cp & 0x3F)), n = 4;
        insert(u, n);
        return true;
    }
    default: return true; /* everything else is the editor's: nothing falls through to the desktop */
    }
}

/* ---- drawing ---- */

/* The next code point at *p (advances it), for the glyph. */
static uint32_t next_cp(size_t *p, size_t end) {
    unsigned char c = (unsigned char)eb_at(&E.b, (*p)++);
    if (c < 0x80) return c;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> more);
    while (more-- > 0 && *p < end && cont(eb_at(&E.b, *p))) cp = cp << 6 | ((unsigned char)eb_at(&E.b, (*p)++) & 0x3F);
    return cp;
}

static void text_at(pal_canvas_t *c, int x, int y, const char *s, pal_color_t fg, pal_color_t bg) {
    const pal_font_t *f = font();
    for (; *s; s++, x += f->w) pal_glyph(c, f, x, y, (unsigned char)*s, fg, bg);
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_font_t *f = font();
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_TERM_BG);
    if (!E.ready) return;
    size_t cur = eb_cursor(&E.b), line = eb_line_of(&E.b, cur), col = col_of(cur), nlines = eb_lines(&E.b);
    /* the title bar: the name, a star when changed, the place */
    char right[32], name[48];
    snprintf(right, sizeof(right), "%s Ln %u, Col %u", E.loading ? "loading" : "", (unsigned)(line + 1), (unsigned)(col + 1));
    snprintf(name, sizeof(name), " %s%s", E.path[0] ? files_base(E.path) : "untitled", E.modified ? " *" : "");
    pal_fill(c, a.x, a.y, a.w, f->h, PAL_C_TERM_FG);
    text_at(c, a.x, a.y, name, PAL_C_TERM_BG, PAL_C_TERM_FG);
    text_at(c, a.x + a.w - (int)strlen(right) * f->w - f->w, a.y, right, PAL_C_TERM_BG, PAL_C_TERM_FG);
    /* the text, a gutter of line numbers on the left */
    int digits = nlines >= 1000 ? 4 : 3;
    int gx = a.x + (digits + 1) * f->w;
    E.rows = (a.h - f->h) / f->h;
    E.cols = (a.x + a.w - gx) / f->w;
    if (E.rows < 1 || E.cols < 4) return;
    if (line < E.top) E.top = line;
    if (line >= E.top + (size_t)E.rows) E.top = line - (size_t)E.rows + 1;
    if (col < E.left) E.left = col;
    if (col >= E.left + (size_t)E.cols) E.left = col - (size_t)E.cols + 1;
    size_t p = eb_line_pos(&E.b, E.top), len = eb_len(&E.b);
    for (int r = 0; r < E.rows && E.top + (size_t)r < nlines; r++) {
        int y = a.y + f->h * (r + 1);
        char num[8];
        snprintf(num, sizeof(num), "%*u", digits, (unsigned)(E.top + (size_t)r + 1));
        text_at(c, a.x, y, num, E.top + (size_t)r == line ? PAL_C_TERM_FG : C_GUTTER, PAL_C_TERM_BG);
        size_t end = eb_line_end(&E.b, p), q = p, k = 0;
        while (q < end || (q == end && q == cur)) {
            size_t at = q;
            uint32_t cp = q < end ? next_cp(&q, end) : ' ';
            if (q == at) q++; /* the cursor cell past the line's end */
            if (cp == '\t') cp = ' ';
            if (k >= E.left && k < E.left + (size_t)E.cols) {
                int x = gx + (int)(k - E.left) * f->w;
                bool here = at == cur;
                pal_glyph(c, f, x, y, cp < 0x20 ? '?' : cp, here ? PAL_C_TERM_BG : PAL_C_TERM_FG, here ? PAL_C_TERM_FG : PAL_C_TERM_BG);
            }
            k++;
            if (at == end) break;
        }
        p = end < len ? end + 1 : len;
    }
}

/* ---- life ---- */

static void pump(pal_view_t *v) {
    (void)v;
    if (E.want_set) {
        E.want_set = false;
        if (!alloc_buf()) {
            psys_toast("Not enough memory for the editor", 1500);
            pal_app_exit();
            return;
        }
        reset(E.want);
        E.ready = true;
        pal_desktop_invalidate();
    }
    if (E.loading) load_step();
}

static void enter(pal_view_t *v) {
    (void)v;
    if (!E.ready && !E.want_set) { /* from the launcher: a new text */
        E.want[0] = 0;
        E.want_set = true;
    }
}

void pal_editor_open(const char *path) {
    if (E.ready && E.modified) { /* the text being edited stays: the user saves or discards it first */
        pal_desktop_open("editor");
        return;
    }
    nema_strlcpy(E.want, path ? path : "", sizeof(E.want));
    E.want_set = true;
    E.ready = false;
    pal_desktop_open("editor");
}

pal_view_t pal_editor_view = {.id = "editor", .title = "Editor", .enter = enter, .draw = draw, .key = key, .pump = pump};
