#include <stdio.h>
#include <string.h>

#include "pal/term.h"
#include "pal/theme.h"
#include "term_int.h"

enum { ST_GROUND, ST_ESC, ST_ESC_INT, ST_CSI, ST_CSI_IGN, ST_STRING, ST_STRING_ESC };

#define MAX_PARAMS 16

struct pal_term {
    int cols, rows;
    nema_owner_t owner;
    pal_cell_t *main_cells, *alt_cells, *cells; /* cells points at whichever screen is active */
    pal_cell_t *sb;                              /* scrollback ring, sb_cap lines of `cols` cells */
    int sb_cap, sb_count, sb_head;
    int view_off;

    int cx, cy;
    bool wrap_pending;
    int top, bot; /* scroll region, inclusive */
    int sx, sy;   /* DECSC saved cursor */
    uint8_t sfg, sbg;
    uint16_t sattr;
    bool saved_origin;

    uint8_t fg, bg;
    uint16_t attr;

    bool autowrap, origin, cursor_visible, app_cursor, bracketed, insert_mode, lnm, alt;
    uint8_t charset[2];
    int gl; /* active charset slot */
    uint8_t tabs[PAL_TERM_MAX_COLS / 8];

    int state;
    int params[MAX_PARAMS];
    int nparams;
    bool have_param, priv;
    char inter;
    uint32_t utf_cp;
    int utf_need;

    uint64_t dirty;
    int cur_r, cur_c; /* where the last draw put the cursor block; cur_r -1: none shown */
    uint32_t bells;
    uint32_t last_ch; /* for REP */
    pal_term_reply_fn reply;
    void *reply_ctx;
};

/* ---- scrollback history ----
 * Written by the terminal task, read by the draw path, freed by the reclaimer from any task. One binary semaphore
 * guards every entry point; the reclaimer only polls it, so it never blocks and cannot deadlock on itself. */
#define LIVE_MAX 8
static struct {
    nema_port_sem_t *lock;
    pal_term_t *live[LIVE_MAX];
} H;

static void reset_body(pal_term_t *t);
static void hist_lock(void) { nema_port_sem_take(H.lock, -1); }
static void hist_unlock(void) { nema_port_sem_give(H.lock); }
static void live_update(pal_term_t *t, bool add) {
    hist_lock();
    for (int i = 0; i < LIVE_MAX; i++) {
        if (add && !H.live[i]) H.live[i] = t;
        if (!add && H.live[i] == t) H.live[i] = NULL;
    }
    hist_unlock();
}

static size_t reclaim_history(void *ctx, size_t wanted) { (void)ctx, (void)wanted; return pal_term_trim_history(); }

static void history_init(void) {
    if (H.lock) return;
    H.lock = nema_port_sem_create(1, 1);
    if (H.lock) nema_mem_add_reclaimer(reclaim_history, NULL, 10);
}

/* ---------------------------------------------------------------- helpers */
static pal_cell_t blank(const pal_term_t *t) {
    pal_cell_t c = {' ', PAL_C_TERM_FG, t->bg, 0};
    return c;
}

static pal_cell_t *at(pal_term_t *t, int col, int row) { return &t->cells[row * t->cols + col]; }

static void dirty_row(pal_term_t *t, int row) { t->dirty |= 1ull << row; }
static void dirty_all(pal_term_t *t) { t->dirty = t->rows >= 64 ? ~0ull : ((1ull << t->rows) - 1); }

static void clear_cells(pal_term_t *t, pal_cell_t *p, int n) {
    pal_cell_t b = blank(t);
    for (int i = 0; i < n; i++) p[i] = b;
}

static void reply(pal_term_t *t, const char *s) {
    if (t->reply) t->reply(t->reply_ctx, s, strlen(s));
}

static void reset_tabs(pal_term_t *t) {
    memset(t->tabs, 0, sizeof(t->tabs));
    for (int i = 8; i < PAL_TERM_MAX_COLS; i += 8) t->tabs[i / 8] |= (uint8_t)(1u << (i % 8));
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------------------------------------------------------------- scrolling */
static void sb_push(pal_term_t *t, const pal_cell_t *line) {
    if (t->sb_cap == 0) return;
    int idx = (t->sb_head + t->sb_count) % t->sb_cap;
    if (t->sb_count == t->sb_cap) {
        t->sb_head = (t->sb_head + 1) % t->sb_cap;
        idx = (t->sb_head + t->sb_count - 1) % t->sb_cap;
    } else {
        t->sb_count++;
    }
    memcpy(t->sb + (size_t)idx * (size_t)t->cols, line, sizeof(pal_cell_t) * (size_t)t->cols);
}

/* Scroll the region up by n lines (content moves up). */
static void scroll_up(pal_term_t *t, int n) {
    int h = t->bot - t->top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    if (!t->alt && t->top == 0 && t->bot == t->rows - 1) {
        for (int i = 0; i < n; i++) sb_push(t, at(t, 0, i));
        if (t->view_off > 0 && t->view_off < t->sb_count) t->view_off = t->view_off + n > t->sb_count ? t->sb_count : t->view_off + n;
    }
    memmove(at(t, 0, t->top), at(t, 0, t->top + n), sizeof(pal_cell_t) * (size_t)t->cols * (size_t)(h - n));
    for (int r = t->bot - n + 1; r <= t->bot; r++) clear_cells(t, at(t, 0, r), t->cols);
    for (int r = t->top; r <= t->bot; r++) dirty_row(t, r);
}

static void scroll_down(pal_term_t *t, int n) {
    int h = t->bot - t->top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    memmove(at(t, 0, t->top + n), at(t, 0, t->top), sizeof(pal_cell_t) * (size_t)t->cols * (size_t)(h - n));
    for (int r = t->top; r < t->top + n; r++) clear_cells(t, at(t, 0, r), t->cols);
    for (int r = t->top; r <= t->bot; r++) dirty_row(t, r);
}

static void linefeed(pal_term_t *t) {
    if (t->cy == t->bot) scroll_up(t, 1);
    else if (t->cy < t->rows - 1) t->cy++;
    if (t->lnm) t->cx = 0;
}

static void reverse_index(pal_term_t *t) {
    if (t->cy == t->top) scroll_down(t, 1);
    else if (t->cy > 0) t->cy--;
}

/* ---------------------------------------------------------------- printing */
static uint32_t dec_graphics(uint32_t c) {
    switch (c) {
    case 'j': return 0x2518; case 'k': return 0x2510; case 'l': return 0x250C; case 'm': return 0x2514;
    case 'n': return 0x253C; case 'q': return 0x2500; case 't': return 0x251C; case 'u': return 0x2524;
    case 'v': return 0x2534; case 'w': return 0x252C; case 'x': return 0x2502; case 'a': return 0x2592;
    case '`': return '+'; case 'f': return 0xB0; case 'g': return 0xB1; case 'o': case 'p': case 'r': case 's': return 0x2500;
    case '~': return 0x2022; case ',': return 0x2190; case '+': return 0x2192; case '.': return 0x2193; case '-': return 0x2191;
    case '0': return 0x2588;
    default: return c;
    }
}

/* Writing over half of a wide character blanks its other half, so no orphan half is left on screen. */
static void unwide(pal_term_t *t, int col, int row) {
    pal_cell_t *c = at(t, col, row);
    if ((c->attr & PAL_TERM_ATTR_WIDE_CONT) && col > 0) {
        pal_cell_t *l = at(t, col - 1, row);
        l->ch = ' ';
        l->attr &= (uint16_t)~PAL_TERM_ATTR_WIDE;
    } else if ((c->attr & PAL_TERM_ATTR_WIDE) && col + 1 < t->cols) {
        pal_cell_t *r = at(t, col + 1, row);
        r->ch = ' ';
        r->attr &= (uint16_t)~PAL_TERM_ATTR_WIDE_CONT;
    }
}

static void put_char(pal_term_t *t, uint32_t cp) {
    if (t->charset[t->gl] == '0') cp = dec_graphics(cp);
    int width = term_cp_width(cp);
    if (width == 0) return; /* a combining mark, joiner or variation selector: it takes no cell (the base shows alone) */
    if (width == 2 && t->cols < 2) width = 1;
    t->last_ch = cp;
    if (width == 2 && !t->wrap_pending && t->cx == t->cols - 1 && t->autowrap) {
        /* a wide character does not fit in the last column: the line ends there, as xterm does */
        pal_cell_t *e = at(t, t->cx, t->cy);
        unwide(t, t->cx, t->cy);
        e->ch = ' ';
        e->attr = t->attr;
        t->wrap_pending = true;
    }
    if (t->wrap_pending) {
        if (t->autowrap) {
            t->cx = 0;
            linefeed(t);
        }
        t->wrap_pending = false;
    }
    if (t->insert_mode && t->cx < t->cols - 1) {
        memmove(at(t, t->cx + 1, t->cy), at(t, t->cx, t->cy), sizeof(pal_cell_t) * (size_t)(t->cols - t->cx - 1));
    }
    unwide(t, t->cx, t->cy);
    pal_cell_t *c = at(t, t->cx, t->cy);
    c->ch = cp;
    c->fg = t->fg;
    c->bg = t->bg;
    c->attr = t->attr;
    if (width == 2 && t->cx + 1 < t->cols) {
        unwide(t, t->cx + 1, t->cy);
        c->attr |= PAL_TERM_ATTR_WIDE;
        pal_cell_t *n = at(t, t->cx + 1, t->cy);
        n->ch = 0;
        n->fg = t->fg;
        n->bg = t->bg;
        n->attr = (uint16_t)(t->attr | PAL_TERM_ATTR_WIDE_CONT);
        t->cx++;
    }
    dirty_row(t, t->cy);
    if (t->cx == t->cols - 1) t->wrap_pending = t->autowrap;
    else t->cx++;
}

/* ---------------------------------------------------------------- CSI handling */
static int param(const pal_term_t *t, int i, int def) {
    if (i >= t->nparams || t->params[i] == 0) return def;
    return t->params[i];
}

static void move_to(pal_term_t *t, int col, int row) {
    int lo = t->origin ? t->top : 0, hi = t->origin ? t->bot : t->rows - 1;
    t->cx = clampi(col, 0, t->cols - 1);
    t->cy = clampi(row + (t->origin ? t->top : 0), lo, hi);
    t->wrap_pending = false;
}

static uint8_t rgb_to_256(int r, int g, int b) {
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 248) return 231;
        return (uint8_t)(232 + (r - 8) / 10);
    }
    int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
    return (uint8_t)(16 + 36 * ri + 6 * gi + bi);
}

static void sgr(pal_term_t *t) {
    if (t->nparams == 0) {
        t->params[0] = 0;
        t->nparams = 1;
    }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) { t->fg = PAL_C_TERM_FG; t->bg = PAL_C_TERM_BG; t->attr = 0; }
        else if (p == 1) t->attr |= PAL_TERM_ATTR_BOLD;
        else if (p == 2) t->attr |= PAL_TERM_ATTR_DIM;
        else if (p == 3) t->attr |= PAL_TERM_ATTR_ITALIC;
        else if (p == 4) t->attr |= PAL_TERM_ATTR_UNDERLINE;
        else if (p == 5 || p == 6) t->attr |= PAL_TERM_ATTR_BLINK;
        else if (p == 7) t->attr |= PAL_TERM_ATTR_REVERSE;
        else if (p == 8) t->attr |= PAL_TERM_ATTR_HIDDEN;
        else if (p == 9) t->attr |= PAL_TERM_ATTR_STRIKE;
        else if (p == 22) t->attr &= (uint16_t) ~(PAL_TERM_ATTR_BOLD | PAL_TERM_ATTR_DIM);
        else if (p == 23) t->attr &= (uint16_t)~PAL_TERM_ATTR_ITALIC;
        else if (p == 24) t->attr &= (uint16_t)~PAL_TERM_ATTR_UNDERLINE;
        else if (p == 25) t->attr &= (uint16_t)~PAL_TERM_ATTR_BLINK;
        else if (p == 27) t->attr &= (uint16_t)~PAL_TERM_ATTR_REVERSE;
        else if (p == 28) t->attr &= (uint16_t)~PAL_TERM_ATTR_HIDDEN;
        else if (p == 29) t->attr &= (uint16_t)~PAL_TERM_ATTR_STRIKE;
        else if (p >= 30 && p <= 37) t->fg = (uint8_t)(p - 30);
        else if (p >= 40 && p <= 47) t->bg = (uint8_t)(p - 40);
        else if (p >= 90 && p <= 97) t->fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = (uint8_t)(p - 100 + 8);
        else if (p == 39) t->fg = PAL_C_TERM_FG;
        else if (p == 49) t->bg = PAL_C_TERM_BG;
        else if (p == 38 || p == 48) {
            uint8_t *dst = p == 38 ? &t->fg : &t->bg;
            if (i + 2 < t->nparams && t->params[i + 1] == 5) {
                *dst = (uint8_t)t->params[i + 2];
                i += 2;
            } else if (i + 4 < t->nparams && t->params[i + 1] == 2) {
                *dst = rgb_to_256(t->params[i + 2], t->params[i + 3], t->params[i + 4]);
                i += 4;
            }
        }
    }
}

static void erase_range(pal_term_t *t, int row, int c0, int c1) { /* inclusive-exclusive */
    if (c1 > t->cols) c1 = t->cols;
    if (c0 < 0) c0 = 0;
    if (c1 > c0) clear_cells(t, at(t, c0, row), c1 - c0);
    dirty_row(t, row);
}

static void set_mode(pal_term_t *t, int mode, bool on) {
    if (!t->priv) {
        if (mode == 4) t->insert_mode = on;
        else if (mode == 20) t->lnm = on;
        return;
    }
    switch (mode) {
    case 1: t->app_cursor = on; break;
    case 6:
        t->origin = on;
        move_to(t, 0, 0);
        break;
    case 7: t->autowrap = on; break;
    case 25: t->cursor_visible = on; break;
    case 2004: t->bracketed = on; break;
    case 47:
    case 1047:
    case 1049: {
        if (on == t->alt) break;
        if (on) {
            if (mode == 1049) {
                t->sx = t->cx, t->sy = t->cy, t->sfg = t->fg, t->sbg = t->bg, t->sattr = t->attr;
            }
            if (!t->alt_cells) {
                t->alt_cells = (pal_cell_t *)nema_alloc(NEMA_MEM_ANY, t->owner,
                                                        sizeof(pal_cell_t) * (size_t)t->cols * (size_t)t->rows);
                if (!t->alt_cells) break;
            }
            t->alt = true;
            t->cells = t->alt_cells;
            clear_cells(t, t->cells, t->cols * t->rows);
            t->view_off = 0;
        } else {
            t->alt = false;
            t->cells = t->main_cells;
            if (mode == 1049) {
                t->cx = clampi(t->sx, 0, t->cols - 1);
                t->cy = clampi(t->sy, 0, t->rows - 1);
                t->fg = t->sfg, t->bg = t->sbg, t->attr = t->sattr;
            }
        }
        t->top = 0;
        t->bot = t->rows - 1;
        t->wrap_pending = false;
        dirty_all(t);
        break;
    }
    default: break; /* mouse modes, blink, etc.: accepted and ignored */
    }
}

static void csi_dispatch(pal_term_t *t, char fin) {
    if (t->inter == '>' || t->inter == '=') { /* secondary device attributes and friends */
        if (fin == 'c') reply(t, "\x1b[>0;10;0c");
        return;
    }
    switch (fin) {
    case 'A': t->cy = clampi(t->cy - param(t, 0, 1), t->origin ? t->top : 0, t->rows - 1); t->wrap_pending = false; break;
    case 'B': t->cy = clampi(t->cy + param(t, 0, 1), 0, t->origin ? t->bot : t->rows - 1); t->wrap_pending = false; break;
    case 'C': case 'a': t->cx = clampi(t->cx + param(t, 0, 1), 0, t->cols - 1); t->wrap_pending = false; break;
    case 'D': t->cx = clampi(t->cx - param(t, 0, 1), 0, t->cols - 1); t->wrap_pending = false; break;
    case 'E': t->cx = 0; t->cy = clampi(t->cy + param(t, 0, 1), 0, t->rows - 1); t->wrap_pending = false; break;
    case 'F': t->cx = 0; t->cy = clampi(t->cy - param(t, 0, 1), 0, t->rows - 1); t->wrap_pending = false; break;
    case 'G': case '`': t->cx = clampi(param(t, 0, 1) - 1, 0, t->cols - 1); t->wrap_pending = false; break;
    case 'd': move_to(t, t->cx, param(t, 0, 1) - 1); break;
    case 'H': case 'f': move_to(t, param(t, 1, 1) - 1, param(t, 0, 1) - 1); break;
    case 'J': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 0) {
            erase_range(t, t->cy, t->cx, t->cols);
            for (int r = t->cy + 1; r < t->rows; r++) erase_range(t, r, 0, t->cols);
        } else if (m == 1) {
            for (int r = 0; r < t->cy; r++) erase_range(t, r, 0, t->cols);
            erase_range(t, t->cy, 0, t->cx + 1);
        } else if (m == 2 || m == 3) {
            for (int r = 0; r < t->rows; r++) erase_range(t, r, 0, t->cols);
            if (m == 3) { t->sb_count = 0; t->sb_head = 0; t->view_off = 0; }
        }
        break;
    }
    case 'K': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 0) erase_range(t, t->cy, t->cx, t->cols);
        else if (m == 1) erase_range(t, t->cy, 0, t->cx + 1);
        else if (m == 2) erase_range(t, t->cy, 0, t->cols);
        break;
    }
    case 'L': /* insert lines */
        if (t->cy >= t->top && t->cy <= t->bot) {
            int save = t->top;
            t->top = t->cy;
            scroll_down(t, param(t, 0, 1));
            t->top = save;
            t->cx = 0;
        }
        break;
    case 'M': /* delete lines */
        if (t->cy >= t->top && t->cy <= t->bot) {
            int save = t->top;
            t->top = t->cy;
            scroll_up(t, param(t, 0, 1));
            t->top = save;
            t->cx = 0;
        }
        break;
    case '@': { /* insert chars */
        int n = clampi(param(t, 0, 1), 1, t->cols - t->cx);
        memmove(at(t, t->cx + n, t->cy), at(t, t->cx, t->cy), sizeof(pal_cell_t) * (size_t)(t->cols - t->cx - n));
        clear_cells(t, at(t, t->cx, t->cy), n);
        dirty_row(t, t->cy);
        break;
    }
    case 'P': { /* delete chars */
        int n = clampi(param(t, 0, 1), 1, t->cols - t->cx);
        memmove(at(t, t->cx, t->cy), at(t, t->cx + n, t->cy), sizeof(pal_cell_t) * (size_t)(t->cols - t->cx - n));
        clear_cells(t, at(t, t->cols - n, t->cy), n);
        dirty_row(t, t->cy);
        break;
    }
    case 'X': erase_range(t, t->cy, t->cx, t->cx + param(t, 0, 1)); break;
    case 'S': scroll_up(t, param(t, 0, 1)); break;
    case 'T': scroll_down(t, param(t, 0, 1)); break;
    case 'b': /* repeat previous character */
        for (int i = param(t, 0, 1); i > 0 && t->last_ch; i--) put_char(t, t->last_ch);
        break;
    case 'm': sgr(t); break;
    case 'r': { /* set scroll region */
        int top = param(t, 0, 1) - 1, bot = (t->nparams > 1 && t->params[1]) ? t->params[1] - 1 : t->rows - 1;
        if (top < 0) top = 0;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) {
            t->top = top;
            t->bot = bot;
            move_to(t, 0, 0);
        }
        break;
    }
    case 's': t->sx = t->cx, t->sy = t->cy; break;
    case 'u': t->cx = clampi(t->sx, 0, t->cols - 1); t->cy = clampi(t->sy, 0, t->rows - 1); t->wrap_pending = false; break;
    case 'g': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 0) t->tabs[t->cx / 8] &= (uint8_t)~(1u << (t->cx % 8));
        else if (m == 3) memset(t->tabs, 0, sizeof(t->tabs));
        break;
    }
    case 'I': for (int i = param(t, 0, 1); i > 0; i--) { do t->cx++; while (t->cx < t->cols - 1 && !(t->tabs[t->cx / 8] & (1u << (t->cx % 8)))); } if (t->cx > t->cols - 1) t->cx = t->cols - 1; break;
    case 'Z': for (int i = param(t, 0, 1); i > 0; i--) { do t->cx--; while (t->cx > 0 && !(t->tabs[t->cx / 8] & (1u << (t->cx % 8)))); } if (t->cx < 0) t->cx = 0; break;
    case 'h': case 'l':
        for (int i = 0; i < (t->nparams ? t->nparams : 1); i++) set_mode(t, t->params[i], fin == 'h');
        break;
    case 'n': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 5) reply(t, "\x1b[0n");
        else if (m == 6) {
            char buf[24];
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR", t->cy + 1 - (t->origin ? t->top : 0), t->cx + 1);
            reply(t, buf);
        }
        break;
    }
    case 'c': reply(t, "\x1b[?1;2c"); break;
    case 't': break; /* window manipulation: ignored */
    case 'q': break; /* cursor style: ignored */
    default: break;
    }
    dirty_row(t, t->cy);
}

/* ---------------------------------------------------------------- control characters and ESC */
static void execute(pal_term_t *t, uint8_t c) {
    switch (c) {
    case 0x07: t->bells++; break;
    case 0x08: if (t->cx > 0) t->cx--; t->wrap_pending = false; break;
    case 0x09: {
        do t->cx++; while (t->cx < t->cols - 1 && !(t->tabs[t->cx / 8] & (1u << (t->cx % 8))));
        if (t->cx > t->cols - 1) t->cx = t->cols - 1;
        t->wrap_pending = false;
        break;
    }
    case 0x0A: case 0x0B: case 0x0C: linefeed(t); t->wrap_pending = false; break;
    case 0x0D: t->cx = 0; t->wrap_pending = false; break;
    case 0x0E: t->gl = 1; break;
    case 0x0F: t->gl = 0; break;
    default: break;
    }
}

static void esc_dispatch(pal_term_t *t, char c) {
    if (t->inter == '(' || t->inter == ')') {
        t->charset[t->inter == '(' ? 0 : 1] = (uint8_t)c;
        return;
    }
    if (t->inter == '#') return; /* DECALN etc. */
    switch (c) {
    case '7': t->sx = t->cx, t->sy = t->cy, t->sfg = t->fg, t->sbg = t->bg, t->sattr = t->attr, t->saved_origin = t->origin; break;
    case '8': t->cx = clampi(t->sx, 0, t->cols - 1); t->cy = clampi(t->sy, 0, t->rows - 1); t->fg = t->sfg; t->bg = t->sbg; t->attr = t->sattr; t->origin = t->saved_origin; t->wrap_pending = false; break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M': reverse_index(t); break;
    case 'H': t->tabs[t->cx / 8] |= (uint8_t)(1u << (t->cx % 8)); break;
    case 'c': reset_body(t); break;
    case '=': case '>': break; /* keypad modes */
    default: break;
    }
}

static void begin_seq(pal_term_t *t, int state) {
    t->state = state;
    t->nparams = 0;
    t->have_param = false;
    t->priv = false;
    t->inter = 0;
    memset(t->params, 0, sizeof(t->params));
}

static void feed(pal_term_t *t, uint8_t b) {
    /* C0 controls act in every state except inside strings. */
    if (t->state != ST_STRING && t->state != ST_STRING_ESC) {
        if (b == 0x1B) { begin_seq(t, ST_ESC); return; }
        if (b == 0x18 || b == 0x1A) { t->state = ST_GROUND; return; } /* CAN, SUB */
        if (b < 0x20) { execute(t, b); return; }
    }
    switch (t->state) {
    case ST_GROUND:
        if (b >= 0x80) { /* UTF-8 */
            if (t->utf_need > 0 && (b & 0xC0) == 0x80) {
                t->utf_cp = (t->utf_cp << 6) | (b & 0x3Fu);
                if (--t->utf_need == 0) put_char(t, t->utf_cp);
                return;
            }
            t->utf_need = 0;
            if ((b & 0xE0) == 0xC0) { t->utf_cp = b & 0x1Fu; t->utf_need = 1; }
            else if ((b & 0xF0) == 0xE0) { t->utf_cp = b & 0x0Fu; t->utf_need = 2; }
            else if ((b & 0xF8) == 0xF0) { t->utf_cp = b & 0x07u; t->utf_need = 3; }
            else put_char(t, 0xFFFD);
            return;
        }
        t->utf_need = 0;
        if (b == 0x7F) return;
        put_char(t, b);
        return;
    case ST_ESC:
        if (b == '[') { begin_seq(t, ST_CSI); return; }
        if (b == ']' || b == 'P' || b == '_' || b == '^' || b == 'X') { begin_seq(t, ST_STRING); return; }
        if (b >= 0x20 && b <= 0x2F) { t->inter = (char)b; t->state = ST_ESC_INT; return; }
        esc_dispatch(t, (char)b);
        t->state = ST_GROUND;
        return;
    case ST_ESC_INT:
        if (b >= 0x20 && b <= 0x2F) return;
        esc_dispatch(t, (char)b);
        t->state = ST_GROUND;
        return;
    case ST_CSI:
        if (b >= '0' && b <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams <= MAX_PARAMS) {
                int *p = &t->params[t->nparams - 1];
                *p = *p * 10 + (b - '0');
                if (*p > 99999) *p = 99999;
            }
            t->have_param = true;
        } else if (b == ';' || b == ':') {
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams < MAX_PARAMS) t->nparams++;
        } else if (b >= '<' && b <= '?') {
            if (b == '?') t->priv = true;
            else t->inter = (char)b;
        } else if (b >= 0x20 && b <= 0x2F) {
            t->inter = (char)b;
        } else if (b >= 0x40 && b <= 0x7E) {
            if (t->nparams == 0 && t->have_param) t->nparams = 1;
            csi_dispatch(t, (char)b);
            t->state = ST_GROUND;
        } else {
            t->state = ST_CSI_IGN;
        }
        return;
    case ST_CSI_IGN:
        if (b >= 0x40 && b <= 0x7E) t->state = ST_GROUND;
        return;
    case ST_STRING: /* OSC, DCS, APC, PM, SOS: swallowed until BEL or ST */
        if (b == 0x07) t->state = ST_GROUND;
        else if (b == 0x1B) t->state = ST_STRING_ESC;
        return;
    case ST_STRING_ESC:
        t->state = (b == '\\') ? ST_GROUND : ST_STRING;
        return;
    default: t->state = ST_GROUND;
    }
}

/* ---------------------------------------------------------------- lifecycle */
void pal_term_reset(pal_term_t *t) { hist_lock(), reset_body(t), hist_unlock(); }

static void reset_body(pal_term_t *t) {
    t->state = ST_GROUND;
    t->fg = PAL_C_TERM_FG;
    t->bg = PAL_C_TERM_BG;
    t->attr = 0;
    t->autowrap = true;
    t->origin = false;
    t->cursor_visible = true;
    t->app_cursor = false;
    t->bracketed = false;
    t->insert_mode = false;
    t->lnm = false;
    t->charset[0] = t->charset[1] = 'B';
    t->gl = 0;
    t->top = 0;
    t->bot = t->rows - 1;
    t->cx = t->cy = 0;
    t->wrap_pending = false;
    t->utf_need = 0;
    if (t->alt) {
        t->alt = false;
        t->cells = t->main_cells;
    }
    reset_tabs(t);
    clear_cells(t, t->main_cells, t->cols * t->rows);
    dirty_all(t);
}

pal_term_t *pal_term_create(int cols, int rows, int scrollback_lines, nema_owner_t owner) {
    history_init(); /* the first terminal is created by the terminal task, before any other exists */
    if (cols < 2 || rows < 2 || cols > PAL_TERM_MAX_COLS || rows > PAL_TERM_MAX_ROWS) return NULL;
    pal_term_t *t = (pal_term_t *)nema_calloc(NEMA_MEM_ANY, owner, sizeof(*t));
    if (!t) return NULL;
    t->cols = cols;
    t->rows = rows;
    t->owner = owner;
    t->cur_r = -1;
    t->sb_cap = scrollback_lines;
    t->main_cells = (pal_cell_t *)nema_alloc(NEMA_MEM_ANY, owner, sizeof(pal_cell_t) * (size_t)cols * (size_t)rows);
    if (scrollback_lines > 0)
        t->sb = (pal_cell_t *)nema_alloc(NEMA_MEM_ANY, owner, sizeof(pal_cell_t) * (size_t)cols * (size_t)scrollback_lines);
    if (!t->main_cells || (scrollback_lines > 0 && !t->sb)) {
        nema_free(t->main_cells);
        nema_free(t->sb);
        nema_free(t);
        return NULL;
    }
    t->cells = t->main_cells;
    reset_body(t);
    live_update(t, true);
    return t;
}

size_t pal_term_trim_history(void) {
    if (!H.lock || nema_port_sem_take(H.lock, 0) != NEMA_OK) return 0; /* busy: try again later */
    size_t freed = 0;
    for (int i = 0; i < LIVE_MAX; i++) {
        pal_term_t *t = H.live[i];
        if (!t || !t->sb) continue;
        freed += sizeof(pal_cell_t) * (size_t)t->cols * (size_t)t->sb_cap;
        nema_free(t->sb);
        t->sb = NULL;
        t->sb_cap = t->sb_count = t->sb_head = t->view_off = 0;
    }
    nema_port_sem_give(H.lock);
    return freed;
}

void pal_term_destroy(pal_term_t *t) {
    if (!t) return;
    live_update(t, false);
    nema_free(t->main_cells);
    nema_free(t->alt_cells);
    nema_free(t->sb);
    nema_free(t);
}

void pal_term_set_reply(pal_term_t *t, pal_term_reply_fn fn, void *ctx) {
    t->reply = fn;
    t->reply_ctx = ctx;
}

void pal_term_write(pal_term_t *t, const void *data, size_t n) {
    hist_lock();
    for (size_t i = 0; i < n; i++) feed(t, ((const uint8_t *)data)[i]);
    hist_unlock();
}

static void resize_body(pal_term_t *t, int cols, int rows);
void pal_term_resize(pal_term_t *t, int cols, int rows) { hist_lock(), resize_body(t, cols, rows), hist_unlock(); }

static void resize_body(pal_term_t *t, int cols, int rows) {
    if (cols < 2 || rows < 2 || cols > PAL_TERM_MAX_COLS || rows > PAL_TERM_MAX_ROWS) return;
    if (cols == t->cols && rows == t->rows) return;
    pal_cell_t *nm = (pal_cell_t *)nema_alloc(NEMA_MEM_ANY, t->owner, sizeof(pal_cell_t) * (size_t)cols * (size_t)rows);
    if (!nm) return;
    pal_cell_t *na = NULL;
    if (t->alt_cells) {
        na = (pal_cell_t *)nema_alloc(NEMA_MEM_ANY, t->owner, sizeof(pal_cell_t) * (size_t)cols * (size_t)rows);
        if (!na) {
            nema_free(nm);
            return;
        }
    }
    pal_cell_t *nsb = NULL;
    if (t->sb_cap > 0) nsb = (pal_cell_t *)nema_alloc(NEMA_MEM_ANY, t->owner, sizeof(pal_cell_t) * (size_t)cols * (size_t)t->sb_cap);
    pal_cell_t b = blank(t);
    int keep_rows = rows < t->rows ? rows : t->rows, keep_cols = cols < t->cols ? cols : t->cols;
    /* when shrinking the height, keep the bottom of the screen: the cursor line stays visible */
    int shift = t->cy >= rows ? t->cy - rows + 1 : 0;
    for (int pass = 0; pass < 2; pass++) {
        pal_cell_t *src = pass == 0 ? t->main_cells : t->alt_cells, *dst = pass == 0 ? nm : na;
        if (!src) continue;
        for (int i = 0; i < cols * rows; i++) dst[i] = b;
        for (int r = 0; r < keep_rows; r++)
            for (int c = 0; c < keep_cols; c++) dst[r * cols + c] = src[(r + shift) * t->cols + c];
    }
    bool was_alt = t->alt;
    nema_free(t->main_cells);
    nema_free(t->alt_cells);
    nema_free(t->sb);
    t->main_cells = nm;
    t->alt_cells = na;
    t->sb = nsb;
    t->sb_count = t->sb_head = t->view_off = 0; /* history does not reflow */
    t->cols = cols;
    t->rows = rows;
    t->cells = was_alt ? na : nm;
    t->top = 0;
    t->bot = rows - 1;
    t->cy = clampi(t->cy - shift, 0, rows - 1);
    t->cx = clampi(t->cx, 0, cols - 1);
    t->wrap_pending = false;
    dirty_all(t);
}

/* ---------------------------------------------------------------- queries */
int pal_term_cols(const pal_term_t *t) { return t->cols; }
int pal_term_rows(const pal_term_t *t) { return t->rows; }
bool pal_term_app_cursor_keys(const pal_term_t *t) { return t->app_cursor; }
bool pal_term_bracketed_paste(const pal_term_t *t) { return t->bracketed; }
bool pal_term_alt_screen(const pal_term_t *t) { return t->alt; }
uint32_t pal_term_bell_count(const pal_term_t *t) { return t->bells; }
int pal_term_view_offset(const pal_term_t *t) { return t->view_off; }
int pal_term_scrollback_used(const pal_term_t *t) { return t->sb_count; }

void pal_term_cursor(const pal_term_t *t, int *col, int *row, bool *visible) {
    *col = t->cx;
    *row = t->cy;
    *visible = t->cursor_visible && t->view_off == 0;
}

static const pal_cell_t *cell_at(const pal_term_t *t, int col, int row) {
    int line = row - t->view_off;
    if (line >= 0) return &t->cells[line * t->cols + col];
    int back = -line; /* 1 = the newest scrollback line */
    if (back > t->sb_count) return &t->cells[col];
    int idx = (t->sb_head + t->sb_count - back) % t->sb_cap;
    return &t->sb[(size_t)idx * (size_t)t->cols + (size_t)col];
}

/* The pointer is valid until the next call that may trim the history: copy the cell out at once. */
const pal_cell_t *pal_term_cell(const pal_term_t *t, int col, int row) {
    hist_lock();
    const pal_cell_t *c = cell_at(t, col, row);
    hist_unlock();
    return c;
}

void pal_term_scroll_view(pal_term_t *t, int delta) {
    hist_lock();
    if (!t->alt) {
        int v = t->view_off + delta;
        t->view_off = clampi(v, 0, t->sb_count);
        dirty_all(t);
    }
    hist_unlock();
}

void pal_term_mark_all(pal_term_t *t) { dirty_all(t); }
void pal_term_mark_row(pal_term_t *t, int row) {
    if (row >= 0 && row < t->rows) dirty_row(t, row);
}

size_t pal_term_row_text(const pal_term_t *t, int row, char *out, size_t cap) {
    size_t n = 0;
    int last = -1;
    hist_lock();
    for (int c = 0; c < t->cols; c++)
        if (cell_at(t, c, row)->ch != ' ') last = c;
    for (int c = 0; c <= last && n + 5 < cap; c++) {
        uint32_t cp = cell_at(t, c, row)->ch;
        if (cell_at(t, c, row)->attr & PAL_TERM_ATTR_WIDE_CONT) continue; /* the second half of a wide character */
        if (cp < 0x80) out[n++] = (char)cp;
        else if (cp < 0x800) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
        else {
            out[n++] = (char)(0xF0 | (cp >> 18)); out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    hist_unlock();
    if (cap) out[n] = 0;
    return n;
}

/* ---------------------------------------------------------------- drawing */
static void cell_colors(const pal_cell_t *c, pal_color_t *fg, pal_color_t *bg) {
    uint8_t f = c->fg, b = c->bg;
    if ((c->attr & PAL_TERM_ATTR_BOLD) && f < 8) f = (uint8_t)(f + 8);
    if ((c->attr & PAL_TERM_ATTR_DIM) && f == PAL_C_TERM_FG) f = PAL_C_DIM;
    if (c->attr & PAL_TERM_ATTR_REVERSE) {
        uint8_t x = f;
        f = b;
        b = x;
    }
    if (c->attr & PAL_TERM_ATTR_HIDDEN) f = b;
    *fg = f;
    *bg = b;
}

int pal_term_draw(pal_term_t *t, pal_canvas_t *c, const pal_font_t *font, int x, int y, bool cursor_on) {
    int drawn = 0;
    int ccol, crow;
    bool cvis;
    pal_term_cursor(t, &ccol, &crow, &cvis);
    /* The cursor block is drawn with its row. When it moves, hides or blinks off without its row changing (a line feed, a cursor
     * move), the row it was on is drawn again so the block does not stay behind, and the row it is on now so it shows there. */
    bool want = cvis && cursor_on && crow >= 0 && crow < t->rows;
    if (t->cur_r >= 0 && t->cur_r < t->rows && (!want || t->cur_r != crow || t->cur_c != ccol)) dirty_row(t, t->cur_r);
    if (want && (t->cur_r != crow || t->cur_c != ccol)) dirty_row(t, crow);
    t->cur_r = want ? crow : -1;
    t->cur_c = ccol;
    hist_lock();
    for (int r = 0; r < t->rows; r++) {
        if (!(t->dirty & (1ull << r))) continue;
        drawn++;
        for (int col = 0; col < t->cols; col++) {
            const pal_cell_t *cell = cell_at(t, col, r);
            pal_color_t fg, bg;
            cell_colors(cell, &fg, &bg);
            if (cvis && cursor_on && col == ccol && r == crow) {
                pal_color_t tmp = fg;
                fg = bg;
                bg = tmp;
            }
            int px = x + col * font->w, py = y + r * font->h;
            if (cell->attr & PAL_TERM_ATTR_WIDE_CONT) {
                if (col == 0) pal_fill(c, px, py, font->w, font->h, bg); /* its first half scrolled off: blank */
                continue;                                                 /* drawn with the first half */
            }
            if ((cell->attr & PAL_TERM_ATTR_WIDE) && col + 1 < t->cols) {
                term_wide_glyph(c, font, px, py, font->w * 2, font->h, cell->ch, fg, bg);
                continue;
            }
            if (!term_special_glyph(c, font, px, py, font->w, font->h, cell->ch, fg, bg)) pal_glyph(c, font, px, py, cell->ch, fg, bg);
            if (cell->attr & PAL_TERM_ATTR_UNDERLINE) pal_fill(c, px, py + font->h - 1, font->w, 1, fg);
            if (cell->attr & PAL_TERM_ATTR_STRIKE) pal_fill(c, px, py + font->h / 2, font->w, 1, fg);
        }
    }
    t->dirty = 0;
    hist_unlock();
    return drawn;
}
