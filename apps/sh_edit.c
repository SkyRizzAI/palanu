#include <stdio.h>
#include <string.h>

#include "pal/history.h"
#include "nema/tty.h"
#include "sh.h"

/* Line editor for a raw TTY: cursor movement, history, kill/yank-less editing, and Tab completion. */

typedef struct {
    sh_t *sh;
    const char *prompt;
    int plen; /* visible width of the prompt */
    char *buf;
    size_t cap;
    int len, cur, off;
    int hpos;               /* history index being browsed; sh->nhist = the line being typed */
    char saved[SH_LINE_MAX]; /* the typed line while browsing history */
    bool tab_again;
} ed_t;

static void out(const char *s, size_t n) { nema_write(1, s, n); }
static void outs(const char *s) { out(s, strlen(s)); }

static int visible_len(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (*s == 0x1b && s[1] == '[') {
            s += 2;
            while (*s && !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) s++;
            if (!*s) break;
            continue;
        }
        n++;
    }
    return n;
}

/* Publish the line being edited to the terminal side (the on-screen keyboard shows it). Cheap: one copy under the tty lock. */
static void publish(const ed_t *e) {
    nema_edit_t ed = {e->buf, (size_t)e->len, (size_t)e->cur};
    nema_ioctl(0, NEMA_TIOC_SETEDIT, &ed);
}

static void redraw(ed_t *e) {
    int avail = e->sh->cols - e->plen - 1;
    if (avail < 8) avail = 8;
    if (e->cur < e->off) e->off = e->cur;
    if (e->cur >= e->off + avail) e->off = e->cur - avail + 1;
    int shown = e->len - e->off < avail ? e->len - e->off : avail;
    char mv[16];
    outs("\r");
    outs(e->prompt);
    out(e->buf + e->off, (size_t)shown);
    outs("\x1b[K");
    int back = e->off + shown - e->cur;
    if (back > 0) {
        int n = snprintf(mv, sizeof(mv), "\x1b[%dD", back);
        out(mv, (size_t)n);
    }
    publish(e);
}

static void insert(ed_t *e, const char *s, int n) {
    if (e->len + n >= (int)e->cap - 1) return;
    memmove(e->buf + e->cur + n, e->buf + e->cur, (size_t)(e->len - e->cur));
    memcpy(e->buf + e->cur, s, (size_t)n);
    e->len += n;
    e->cur += n;
}

static void erase(ed_t *e, int from, int to) { /* [from, to) */
    memmove(e->buf + from, e->buf + to, (size_t)(e->len - to));
    e->len -= to - from;
    if (e->cur > from) e->cur = e->cur >= to ? e->cur - (to - from) : from;
}

void sh_hist_add(sh_t *sh, const char *line) {
    if (!*line) return;
    if (sh->interactive) pal_history_add(line); /* kept across restarts; written while the prompt is idle */
    if (sh->nhist && strcmp(sh->hist[sh->nhist - 1], line) == 0) return;
    char *copy = (char *)nema_alloc(NEMA_MEM_ANY, sh->owner, strlen(line) + 1);
    if (!copy) return;
    strcpy(copy, line);
    if (sh->nhist == SH_HIST_MAX) {
        nema_free(sh->hist[0]);
        memmove(&sh->hist[0], &sh->hist[1], sizeof(char *) * (SH_HIST_MAX - 1));
        sh->nhist--;
    }
    sh->hist[sh->nhist++] = copy;
}

static void set_line(ed_t *e, const char *s) {
    e->len = (int)strlen(s);
    if (e->len >= (int)e->cap) e->len = (int)e->cap - 1;
    memcpy(e->buf, s, (size_t)e->len);
    e->cur = e->len;
}

static void hist_move(ed_t *e, int dir) {
    int np = e->hpos + dir;
    if (np < 0 || np > e->sh->nhist) return;
    if (e->hpos == e->sh->nhist) {
        memcpy(e->saved, e->buf, (size_t)e->len);
        e->saved[e->len] = 0;
    }
    e->hpos = np;
    if (np == e->sh->nhist) set_line(e, e->saved);
    else set_line(e, e->sh->hist[np]);
}

static void word_back(ed_t *e) {
    int p = e->cur;
    while (p > 0 && e->buf[p - 1] == ' ') p--;
    while (p > 0 && e->buf[p - 1] != ' ') p--;
    erase(e, p, e->cur);
}

/* ---- Tab completion ---- */
static int common_prefix(const sh_comp_t *c) {
    int n = (int)strlen(c->c[0].name);
    for (int i = 1; i < c->n; i++) {
        int j = 0;
        while (j < n && c->c[i].name[j] == c->c[0].name[j]) j++;
        n = j;
    }
    return n;
}

static void complete(ed_t *e) {
    sh_comp_t *c = &e->sh->comp;
    e->buf[e->len] = 0;
    sh_complete(e->sh, e->buf, e->cur, c);
    if (c->n == 0) {
        e->tab_again = false;
        outs("\a");
        return;
    }
    int typed = e->cur - c->start;
    int common = common_prefix(c);
    if (common > typed) {
        insert(e, c->c[0].name + typed, common - typed);
        if (c->n == 1) {
            char last = c->c[0].name[common - 1];
            if (last != '/') insert(e, " ", 1);
        }
        e->tab_again = false;
        redraw(e);
        return;
    }
    if (c->n == 1) {
        if (c->c[0].name[strlen(c->c[0].name) - 1] != '/') insert(e, " ", 1);
        redraw(e);
        return;
    }
    if (!e->tab_again) {
        e->tab_again = true;
        outs("\a");
        return;
    }
    /* second Tab: list the candidates in columns, then redraw the prompt below them */
    int w = 0;
    for (int i = 0; i < c->n; i++) {
        int l = (int)strlen(c->c[i].name);
        if (l > w) w = l;
    }
    w += 2;
    int per = e->sh->cols / w;
    if (per < 1) per = 1;
    outs("\r\n");
    for (int i = 0; i < c->n; i++) {
        char cell[NEMA_NAME_MAX + 8];
        int n = snprintf(cell, sizeof(cell), "%-*s", w, c->c[i].name);
        out(cell, (size_t)n);
        if ((i + 1) % per == 0 || i + 1 == c->n) outs("\r\n");
    }
    e->tab_again = false;
    redraw(e);
}

/* ---- input ---- */
static int getb(int32_t timeout_ms) {
    unsigned char b;
    int r = nema_read_timeout(0, &b, 1, timeout_ms);
    /* a killed shell (CANCELED) or a closed terminal (0) ends the input; only a signal interrupt means "drop the line" */
    if (r == NEMA_ERR_CANCELED) return -1;
    /* a zero wait (the history was due already: a command ran longer than its delay) answers AGAIN, not TIMEOUT; taken as an
     * interrupt it left the history unwritten and the shell asking again at once, forever (found on the board after `update`) */
    if (r == NEMA_ERR_AGAIN && timeout_ms == 0) return -2;
    return r == 1 ? b : (r == 0 ? -1 : (r == NEMA_ERR_TIMEOUT ? -2 : -3));
}

static int readline_body(sh_t *sh, const char *prompt, char *outbuf, size_t cap) {
    ed_t e = {.sh = sh, .prompt = prompt, .buf = outbuf, .cap = cap, .hpos = sh->nhist};
    e.plen = visible_len(prompt);
    e.buf[0] = 0;
    redraw(&e);
    for (;;) {
        /* An interactive shell writes the history file once its prompt has been quiet for a moment: never while a
         * command runs or keys arrive (pal/history.h). */
        int32_t wait = sh->interactive ? pal_history_flush_in_ms() : -1;
        int c = getb(wait);
        if (c == -2) {
            if (wait >= 0) pal_history_flush();
            continue;
        }
        if (c == -3) { /* interrupted by a signal: drop the line */
            nema_sig_take(NEMA_SIG_INT | NEMA_SIG_WINCH);
            continue;
        }
        if (c < 0) return -1;
        if (c != '\t') e.tab_again = false;
        switch (c) {
        case '\r':
        case '\n':
            e.buf[e.len] = 0;
            outs("\r\n");
            return e.len;
        case 0x03: /* Ctrl-C */
            outs("^C\r\n");
            e.buf[0] = 0;
            return 0;
        case 0x04: /* Ctrl-D */
            if (e.len == 0) {
                outs("\r\n");
                return -1;
            }
            if (e.cur < e.len) erase(&e, e.cur, e.cur + 1);
            break;
        case 0x01: e.cur = 0; break;
        case 0x05: e.cur = e.len; break;
        case 0x02: if (e.cur > 0) e.cur--; break;
        case 0x06: if (e.cur < e.len) e.cur++; break;
        case 0x0B: erase(&e, e.cur, e.len); break;
        case 0x15: erase(&e, 0, e.cur); break;
        case 0x17: word_back(&e); break;
        case 0x0C: outs("\x1b[2J\x1b[H"); break;
        case 0x10: hist_move(&e, -1); break;
        case 0x0E: hist_move(&e, 1); break;
        case 0x7F:
        case 0x08:
            if (e.cur > 0) erase(&e, e.cur - 1, e.cur);
            break;
        case '\t': complete(&e); continue;
        case 0x1B: {
            int c2 = getb(40);
            if (c2 != '[' && c2 != 'O') break;
            int num = 0, f;
            while ((f = getb(40)) >= '0' && f <= '9') num = num * 10 + (f - '0');
            if (f == 'A') hist_move(&e, -1);
            else if (f == 'B') hist_move(&e, 1);
            else if (f == 'C') { if (e.cur < e.len) e.cur++; }
            else if (f == 'D') { if (e.cur > 0) e.cur--; }
            else if (f == 'H') e.cur = 0;
            else if (f == 'F') e.cur = e.len;
            else if (f == '~') {
                if (num == 3 && e.cur < e.len) erase(&e, e.cur, e.cur + 1);
                else if (num == 1) e.cur = 0;
                else if (num == 4) e.cur = e.len;
            }
            break;
        }
        default:
            if (c >= 0x20) {
                char ch = (char)c;
                insert(&e, &ch, 1);
            }
        }
        redraw(&e);
    }
}

int sh_readline(sh_t *sh, const char *prompt, char *outbuf, size_t cap) {
    int r = readline_body(sh, prompt, outbuf, cap);
    nema_ioctl(0, NEMA_TIOC_CLREDIT, NULL); /* submitted, cancelled or ended: no line is being edited */
    return r;
}
