#define EV_NONE 0
#define EV_KEY 1
#define EV_PASTE 2
#define ESC_MS 40
#define MAXC PALTUI_MAX_COLS
#define CAP_CONT TUI_A_CONT

/* ---- plain helpers: no libc, no heap ---- */
static void tzero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}
static void tcopy(void *d, const void *s, uint32_t n) {
    uint8_t *o = (uint8_t *)d;
    const uint8_t *i = (const uint8_t *)s;
    while (n--) *o++ = *i++;
}
static void tmove(uint8_t *d, const uint8_t *s, uint32_t n) { /* overlapping: copies backwards when d is after s */
    if (d <= s) tcopy(d, s, n);
    else while (n--) d[n] = s[n];
}
static uint32_t tslen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static int teq(const uint8_t *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}
static uint32_t id_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h ? h : 1;
}

/* width of a code point in columns: 2 for the East Asian wide blocks, 1 otherwise */
static int cp_width(uint32_t cp) {
    if (cp < 0x1100) return 1;
    if (cp <= 0x115F || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6))
        return 2;
    return 1;
}
static int u8len(uint8_t c) { return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

/* Decodes one code point at s (not past end). *n gets its byte length. Bad UTF-8 and non-BMP give '?'. */
static uint32_t u8_dec(const char *s, const char *end, int *n) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t cp = p[0];
    int need = u8len(p[0]) - 1;
    *n = 1;
    if (cp < 0x80) return cp;
    if (need == 0 || end - s < need + 1) return '?';
    cp &= (0x3F >> need);
    for (int i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *n = need + 1;
    return cp > 0xFFFF ? '?' : cp;
}
static int u8_enc(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}
static int num_str(char *o, uint32_t v) {
    char b[12];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) o[i] = b[n - 1 - i];
    return n;
}

/* ---- output: batched, flushed through the backend ---- */
static void flush(tui_t *t) {
    uint32_t off = 0;
    while (off < (uint32_t)t->outn) {
        int32_t r = t->io.write(t->out + off, (uint32_t)t->outn - off);
        if (r <= 0) break;
        off += (uint32_t)r;
    }
    t->outn = 0;
}
static void o_bytes(tui_t *t, const char *s, int n) {
    while (n > 0) {
        int room = PALTUI_OUT_BYTES - t->outn;
        if (room == 0) { flush(t); continue; }
        int k = n < room ? n : room;
        tcopy(t->out + t->outn, s, (uint32_t)k);
        t->outn += k;
        s += k;
        n -= k;
    }
}
static void o_str(tui_t *t, const char *s) { o_bytes(t, s, (int)tslen(s)); }
static void o_num(tui_t *t, uint32_t v) {
    char b[12];
    int n = num_str(b, v);
    o_bytes(t, b, n);
}
static void cup(tui_t *t, int x, int y) {
    o_str(t, "\x1b[");
    o_num(t, (uint32_t)y + 1);
    o_str(t, ";");
    o_num(t, (uint32_t)x + 1);
    o_str(t, "H");
}

/* SGR text per semantic style for each depth (2: 256 colours, 1: 16 colours, 0: mono). "" is the plain reset. */
static const char *const sgr256[8] = {"", "1;38;5;81", "1;38;5;231;48;5;25", "2;38;5;244",
                                      "1;38;5;203", "38;5;114", "38;5;215", "38;5;252;48;5;237"};
static const char *const sgr16[8] = {"", "1;96", "1;97;44", "90", "1;91", "92", "93", "97;100"};
static const char *const sgrmono[8] = {"", "1", "1;7", "2", "1;4", "1", "1", "4"};

static void sgr(tui_t *t, int style, int cur) {
    const char *s = t->depth == 2 ? sgr256[style] : t->depth == 1 ? sgr16[style] : sgrmono[style];
    o_str(t, "\x1b[0");
    if (*s) { o_str(t, ";"); o_str(t, s); }
    if (cur) o_str(t, ";7");
    o_str(t, "m");
}

/* ---- cells ---- */
static tui_cell_t *bcell(tui_t *t, int x, int y) { return &t->cells[t->cur][y * MAXC + x]; }
static tui_cell_t *fcell(tui_t *t, int x, int y) { return &t->cells[1 - t->cur][y * MAXC + x]; }

/* Writes one character at an absolute position (screen bounds only). Keeps wide pairs whole. */
static void cell_abs(tui_t *t, int x, int y, uint32_t cp, int style, int attr) {
    if (x < 0 || y < 0 || x >= t->cols || y >= t->rows) return;
    if (cp == 0) cp = ' ';
    int w = cp_width(cp) == 2 && x + 1 < t->cols ? 2 : 1;
    if (cp_width(cp) == 2 && w == 1) cp = '?';
    tui_cell_t *c = bcell(t, x, y);
    if ((c->attr & CAP_CONT) && x > 0) { c[-1].cp = ' '; c[-1].style = TUI_NORMAL; c[-1].attr = 0; }
    if ((c->attr & TUI_A_WIDE) && x + 1 < t->cols) { c[1].cp = ' '; c[1].style = TUI_NORMAL; c[1].attr = 0; }
    if (w == 2 && (c[1].attr & TUI_A_WIDE) && x + 2 < t->cols) { c[2].cp = ' '; c[2].style = TUI_NORMAL; c[2].attr = 0; }
    c->cp = (uint16_t)cp;
    c->style = (uint8_t)style;
    c->attr = (uint8_t)(attr | (w == 2 ? TUI_A_WIDE : 0));
    if (w == 2) {
        c[1].cp = 0;
        c[1].style = (uint8_t)style;
        c[1].attr = CAP_CONT;
    }
}
/* Same, inside the current region; returns the columns the character takes (0 if it is outside). */
static int draw_cp(tui_t *t, int x, int y, uint32_t cp, int style, int attr) {
    int w = cp_width(cp);
    if (y < t->ry || y >= t->ry + t->rh || x < t->rx || x >= t->rx + t->rw) return w;
    if (w == 2 && x + 1 >= t->rx + t->rw) cp = '?';
    cell_abs(t, x, y, cp, style, attr);
    t->w_frame++; /* widget writes only: the bars and frames are not counted */
    return w;
}
/* Text on one row, at most maxw columns; control characters become spaces. Returns the columns used. */
static int text_put(tui_t *t, int x, int y, const char *s, int len, int maxw, int style, int absolute) {
    const char *e = s + len;
    int used = 0;
    while (s < e) {
        int n;
        uint32_t cp = u8_dec(s, e, &n);
        if (cp < 0x20) cp = ' ';
        int cw = cp_width(cp);
        if (used + cw > maxw) break;
        if (absolute) cell_abs(t, x + used, y, cp, style, 0);
        else draw_cp(t, x + used, y, cp, style, 0);
        used += cw;
        s += n;
    }
    return used;
}
static int str_w(const char *s) {
    int w = 0, n;
    const char *e = s + tslen(s);
    while (s < e) { w += cp_width(u8_dec(s, e, &n)); s += n; }
    return w;
}
static void fill(tui_t *t, int x, int y, int w, int style) {
    for (int i = 0; i < w; i++) draw_cp(t, x + i, y, ' ', style, 0);
}
static int can_draw(const tui_t *t) { return t->rw > 0 && t->cy < t->ry + t->rh; }

/* ---- focus and ids ---- */
static void rotate_ids(tui_t *t) {
    tcopy(t->prev, t->ids, sizeof(t->ids));
    t->nprev = t->nids;
    t->nids = 0;
}
/* Registers a focusable widget for this frame; true when it has the focus. */
static int reg_focus(tui_t *t, uint32_t h) {
    if (t->nids < PALTUI_MAX_IDS) t->ids[t->nids++] = h;
    if (t->focus == 0) t->focus = h;
    return t->focus == h;
}
static void cycle_focus(tui_t *t, int back) {
    if (t->nprev == 0) return;
    int idx = -1;
    for (int i = 0; i < t->nprev; i++)
        if (t->prev[i] == t->focus) idx = i;
    if (idx < 0) t->focus = t->prev[0];
    else t->focus = t->prev[back ? (idx + t->nprev - 1) % t->nprev : (idx + 1) % t->nprev];
}
/* Per-widget state, found by id or replacing the least recently used slot. */
static tui_slot_t *slot(tui_t *t, uint32_t h) {
    int best = 0;
    for (int i = 0; i < PALTUI_SLOTS; i++) {
        if (t->slots[i].id == h) { t->slots[i].seq = ++t->seq; return &t->slots[i]; }
        if (t->slots[i].seq < t->slots[best].seq) best = i;
    }
    tzero(&t->slots[best], sizeof(tui_slot_t));
    t->slots[best].id = h;
    t->slots[best].seq = ++t->seq;
    return &t->slots[best];
}

/* ---- input decoding ---- */
static void set_key(tui_t *t, uint16_t sym, uint32_t cp, uint8_t mods) {
    t->ev = EV_KEY;
    t->key.sym = sym;
    t->key.cp = cp;
    t->key.mods = mods;
}
static int ss3_key(tui_t *t, uint8_t f) {
    switch (f) {
    case 'A': set_key(t, TUI_K_UP, 0, 0); return 1;
    case 'B': set_key(t, TUI_K_DOWN, 0, 0); return 1;
    case 'C': set_key(t, TUI_K_RIGHT, 0, 0); return 1;
    case 'D': set_key(t, TUI_K_LEFT, 0, 0); return 1;
    case 'H': set_key(t, TUI_K_HOME, 0, 0); return 1;
    case 'F': set_key(t, TUI_K_END, 0, 0); return 1;
    case 'P': case 'Q': case 'R': case 'S': set_key(t, (uint16_t)(TUI_K_F1 + f - 'P'), 0, 0); return 1;
    default: return 0;
    }
}
/* CSI: ESC [ params final. Bracketed paste markers switch paste mode. *used gets the bytes taken. */
static int decode_csi(tui_t *t, const uint8_t *p, int avail, int *used) {
    int i = 2, v[4] = {0, 0, 0, 0}, nv = 0;
    while (i < avail && p[i] >= 0x30 && p[i] <= 0x3F) {
        uint8_t c = p[i++];
        if (c >= '0' && c <= '9') { if (nv < 4) v[nv] = v[nv] * 10 + (c - '0'); }
        else if (c == ';') nv++;
    }
    while (i < avail && p[i] >= 0x20 && p[i] <= 0x2F) i++;
    if (i >= avail) {
        if (avail < 32) return -1;
        *used = avail;
        return 0;
    }
    uint8_t f = p[i];
    *used = i + 1;
    if (f < 0x40 || f > 0x7E) return 0;
    int p1 = v[0], p2 = v[1];
    uint8_t mods = p2 >= 2 ? (uint8_t)((p2 - 1) & 7) : 0;
    if (f == '~' && p1 == 200) { t->paste_mode = 1; return 0; }
    if (f == '~' && p1 == 201) return 0;
    switch (f) {
    case 'A': case 'B': case 'C': case 'D': case 'H': case 'F': case 'P': case 'Q': case 'R': case 'S':
        if (!ss3_key(t, f)) return 0;
        t->key.mods = mods;
        return 1;
    case 'Z': set_key(t, TUI_K_TAB, 0, mods | TUI_MOD_SHIFT); return 1;
    case '~': break;
    default: return 0;
    }
    uint16_t sym = 0;
    if (p1 == 1 || p1 == 7) sym = TUI_K_HOME;
    else if (p1 == 2) sym = TUI_K_INSERT;
    else if (p1 == 3) sym = TUI_K_DELETE;
    else if (p1 == 4 || p1 == 8) sym = TUI_K_END;
    else if (p1 == 5) sym = TUI_K_PGUP;
    else if (p1 == 6) sym = TUI_K_PGDN;
    else if (p1 >= 11 && p1 <= 15) sym = (uint16_t)(TUI_K_F1 + p1 - 11);
    else if (p1 >= 17 && p1 <= 21) sym = (uint16_t)(TUI_K_F1 + p1 - 11);
    else if (p1 == 23 || p1 == 24) sym = (uint16_t)(TUI_K_F1 + p1 - 12);
    if (!sym) return 0;
    set_key(t, sym, 0, mods);
    return 1;
}
/* Inside a bracketed paste: hands out chunks that end at a character boundary, before the end marker. */
static int decode_paste(tui_t *t, const uint8_t *p, int avail, int *used) {
    static const char end[] = "\x1b[201~";
    for (int i = 0; i + 6 <= avail; i++) {
        if (p[i] != 0x1b || !teq(p + i, end, 6)) continue;
        if (i == 0) { t->paste_mode = 0; *used = 6; return 0; }
        t->ev = EV_PASTE;
        t->paste = (const char *)p;
        t->paste_len = (uint32_t)i;
        *used = i;
        return 1;
    }
    int keep = 0;
    for (int k = 5; k >= 1 && !keep; k--)
        if (k <= avail && p[avail - k] == 0x1b && teq(p + avail - k, end, k)) keep = k;
    int cut = avail - keep;
    int j = cut;
    while (j > 0 && (p[j - 1] & 0xC0) == 0x80) j--;
    if (j < cut && j + u8len(p[j]) > cut) cut = j;
    if (cut == 0) return -1;
    t->ev = EV_PASTE;
    t->paste = (const char *)p;
    t->paste_len = (uint32_t)cut;
    *used = cut;
    return 1;
}
/* Decodes one event from the front of the input. 1: event set, 0: bytes taken and nothing to report, -1: need more. */
static int decode(tui_t *t, const uint8_t *p, int avail, int *used) {
    *used = 1;
    if (t->paste_mode) return decode_paste(t, p, avail, used);
    uint8_t c = p[0];
    if (c == 0x1b) {
        if (avail < 2) return -1;
        if (p[1] == '[') return decode_csi(t, p, avail, used);
        if (p[1] == 'O') {
            if (avail < 3) return -1;
            *used = 3;
            if (!ss3_key(t, p[2])) return 0;
            return 1;
        }
        if (p[1] >= 0x20 && p[1] != 0x7f) { /* Alt + character */
            if (avail < 1 + u8len(p[1])) return -1;
            int n;
            uint32_t cp = u8_dec((const char *)p + 1, (const char *)p + avail, &n);
            *used = 1 + n;
            set_key(t, TUI_K_NONE, cp, TUI_MOD_ALT);
            return 1;
        }
        set_key(t, TUI_K_ESC, 0, 0);
        return 1;
    }
    if (c == '\r' || c == '\n') { set_key(t, TUI_K_ENTER, 0, 0); return 1; }
    if (c == '\t') { set_key(t, TUI_K_TAB, 0, 0); return 1; }
    if (c == 0x7f || c == 0x08) { set_key(t, TUI_K_BACKSPACE, 0, 0); return 1; }
    if (c < 0x20) {
        uint32_t cp = (c >= 1 && c <= 26) ? 'a' + c - 1 : (c >= 0x1c && c <= 0x1f) ? c + 0x40 : ' ';
        set_key(t, TUI_K_NONE, cp, TUI_MOD_CTRL);
        return 1;
    }
    if (c >= 0x80) {
        if (avail < u8len(c)) return -1;
        int n;
        uint32_t cp = u8_dec((const char *)p, (const char *)p + avail, &n);
        *used = n;
        set_key(t, TUI_K_NONE, cp, 0);
        return 1;
    }
    set_key(t, TUI_K_NONE, c, 0);
    return 1;
}

/* Waits for the next event. 1: an event is in t->ev, 0: timeout, 2: resize, 3: extra handle ready, -1: stop. */
static int next_event(tui_t *t, int32_t timeout) {
    int32_t start = t->io.now_ms(), deadline = start + timeout;
    for (;;) {
        if (t->in_pos < t->in_len) {
            int used = 0;
            int r = decode(t, t->in + t->in_pos, t->in_len - t->in_pos, &used);
            if (r >= 0) {
                t->in_pos += used;
                if (r == 1) return 1;
                continue;
            }
        }
        int32_t now = t->io.now_ms();
        int lone = !t->paste_mode && t->in_len - t->in_pos == 1 && t->in[t->in_pos] == 0x1b;
        if (lone) {
            if (!t->esc_pending) { t->esc_pending = 1; t->esc_at = now; }
            if (now - t->esc_at >= ESC_MS) { /* a bare ESC: no more bytes came in time */
                t->in_pos++;
                t->esc_pending = 0;
                set_key(t, TUI_K_ESC, 0, 0);
                return 1;
            }
        } else t->esc_pending = 0;
        int32_t wait = timeout < 0 ? -1 : (deadline - now > 0 ? deadline - now : 0);
        if (lone) {
            int32_t w2 = ESC_MS - (now - t->esc_at);
            if (wait < 0 || wait > w2) wait = w2;
        }
        if (t->in_pos > 0) {
            tmove(t->in, t->in + t->in_pos, (uint32_t)(t->in_len - t->in_pos));
            t->in_len -= t->in_pos;
            t->in_pos = 0;
        }
        if (t->in_len == PALTUI_IN_BYTES) t->in_pos = t->in_len = 0; /* garbage too long to ever decode */
        int32_t n = t->io.read(t->in + t->in_len, (uint32_t)(PALTUI_IN_BYTES - t->in_len), wait);
        if (n > 0) { t->in_len += n; t->esc_pending = 0; continue; }
        if (n == 0) {
            if (timeout >= 0 && t->io.now_ms() - deadline >= 0) return 0;
            continue;
        }
        if (n == TUI_IO_WAKE_RESIZE) return 2;
        if (n == TUI_IO_WAKE_EXTRA) return 3;
        return -1;
    }
}

/* Clears the back buffer and resets the region to the whole screen. */
static void clear_back(tui_t *t) {
    for (int y = 0; y < t->rows; y++)
        for (int x = 0; x < t->cols; x++) {
            tui_cell_t *c = bcell(t, x, y);
            c->cp = ' ';
            c->style = TUI_NORMAL;
            c->attr = 0;
        }
    t->rx = 0;
    t->ry = 0;
    t->rw = t->cols;
    t->rh = t->rows;
    t->cy = 0;
}
static int begin(tui_t *t, int32_t timeout, int clear) {
    rotate_ids(t);
    t->ev = EV_NONE;
    t->consumed = 0;
    if (t->nprev) {
        int known = 0;
        for (int i = 0; i < t->nprev; i++)
            if (t->prev[i] == t->focus) known = 1;
        if (!known) t->focus = t->prev[0];
    }
    int r = next_event(t, timeout);
    if (r < 0) { tui_close(t); return 0; }
    int c = 0, rw = 0;
    if (t->io.size(&c, &rw) == 0 && c > 0 && rw > 0) {
        if (c > MAXC) c = MAXC;
        if (rw > PALTUI_MAX_ROWS) rw = PALTUI_MAX_ROWS;
        if (c != t->cols || rw != t->rows) { t->cols = c; t->rows = rw; t->full = 1; }
    }
    if (t->ev == EV_KEY && t->key.sym == TUI_K_TAB) {
        cycle_focus(t, t->key.mods & TUI_MOD_SHIFT);
        t->consumed = 1;
    }
    if (clear) {
        clear_back(t);
        t->w_frame = 0;
    }
    return 1;
}

/* ---- terminal modes ---- */
static void tty_mode(tui_t *t, int raw);
static void enter_screen(tui_t *t) {
    o_str(t, "\x1b[?1049h\x1b[2J\x1b[H\x1b[0m\x1b[?25l\x1b[?2004h");
    t->ocx = -1;
    t->ostate = 0;
}
static void leave_screen(tui_t *t) { o_str(t, "\x1b[0m\x1b[?2004l\x1b[?25h\x1b[?1049l"); }

int tui_open(tui_t *t, const tui_io_t *io) {
    tzero(t, sizeof(*t));
    t->io = *io;
    int c = 0, rw = 0;
    if (t->io.size(&c, &rw) != 0 || c <= 0 || rw <= 0) { c = 80; rw = 24; }
    t->cols = c > MAXC ? MAXC : c;
    t->rows = rw > PALTUI_MAX_ROWS ? PALTUI_MAX_ROWS : rw;
    int caps = t->io.caps();
    t->utf8 = (caps & TUI_CAP_UTF8) != 0;
    t->depth = (caps & TUI_CAP_256) ? 2 : (caps & TUI_CAP_16) ? 1 : 0;
    t->open = 1;
    t->full = 1;
    tty_mode(t, 1);
    enter_screen(t);
    flush(t);
    return 0;
}
void tui_close(tui_t *t) {
    if (!t->open) return;
    if (!t->suspended) leave_screen(t);
    flush(t);
    tty_mode(t, 0);
    t->open = 0;
}
int tui_suspend(tui_t *t) {
    if (!t->open || t->suspended) return -1;
    leave_screen(t);
    flush(t);
    tty_mode(t, 0);
    t->suspended = 1;
    return 0;
}
int tui_resume(tui_t *t) {
    if (!t->open || !t->suspended) return -1;
    tty_mode(t, 1);
    enter_screen(t);
    flush(t);
    t->suspended = 0;
    t->full = 1;
    return 0;
}
void tui_size(const tui_t *t, int *cols, int *rows) {
    *cols = t->cols;
    *rows = t->rows;
}

int tui_frame(tui_t *t, int32_t timeout_ms) {
    if (!t->open) return TUI_QUIT;
    return begin(t, timeout_ms, 1) ? TUI_RUN : TUI_QUIT;
}
void tui_end(tui_t *t) {
    if (!t->open || t->suspended) return;
    if (t->full) {
        o_str(t, "\x1b[2J");
        for (int y = 0; y < t->rows; y++)
            for (int x = 0; x < t->cols; x++) {
                tui_cell_t *f = fcell(t, x, y);
                f->cp = 0xFFFF; /* never drawn: every cell differs once */
                f->style = 0xFF;
                f->attr = 0;
            }
        t->full = 0;
        t->ocx = -1;
        t->ostate = -1;
    }
    for (int y = 0; y < t->rows; y++) {
        for (int x = 0; x < t->cols;) {
            const tui_cell_t *b = bcell(t, x, y), *f = fcell(t, x, y);
            if (b->cp == f->cp && b->style == f->style && b->attr == f->attr) { x++; continue; }
            if (b->attr & CAP_CONT) { x++; continue; } /* drawn with its lead cell */
            if (t->ocx != x || t->ocy != y) cup(t, x, y);
            int cur = (b->attr & TUI_A_CURSOR) != 0;
            int key = b->style | (cur << 8);
            if (key != t->ostate) { sgr(t, b->style, cur); t->ostate = key; }
            char enc[4];
            o_bytes(t, enc, u8_enc(b->cp ? b->cp : ' ', enc));
            x += (b->attr & TUI_A_WIDE) ? 2 : 1;
            t->ocx = x < t->cols ? x : -1;
            t->ocy = y;
        }
    }
    if (t->ostate != 0) { o_str(t, "\x1b[0m"); t->ostate = 0; }
    flush(t);
    t->cur = 1 - t->cur;
    t->w_last = t->w_frame;
    if (t->w_frame > t->w_max) t->w_max = t->w_frame;
}
void tui_stats(const tui_t *t, uint32_t *last, uint32_t *max) {
    *last = t->w_last;
    *max = t->w_max;
}
int tui_shown(const tui_t *t, int x, int y, uint32_t *cp, uint8_t *style, uint8_t *attr) {
    if (x < 0 || y < 0 || x >= t->cols || y >= t->rows) return -1;
    const tui_cell_t *c = &t->cells[1 - t->cur][y * MAXC + x];
    *cp = c->cp;
    *style = c->style;
    *attr = c->attr;
    return 0;
}

const tui_key_t *tui_key(tui_t *t) {
    if (t->ev != EV_KEY || t->consumed) return 0;
    t->consumed = 1;
    return &t->key;
}
const char *tui_paste(tui_t *t, uint32_t *len) {
    if (t->ev != EV_PASTE || t->consumed) return 0;
    t->consumed = 1;
    *len = t->paste_len;
    return t->paste;
}
static const tui_key_t *peek_key(const tui_t *t) { return (t->ev == EV_KEY && !t->consumed) ? &t->key : 0; }
static void use_key(tui_t *t) { t->consumed = 1; }

#ifdef PALTUI_WITH_PAL
/* ---- the default backend: stdin/stdout, the SDK's terminal calls and the event queue ---- */
static struct {
    int init, ev;
    int32_t h[PALTUI_EXTRA];
    uint16_t want[PALTUI_EXTRA], got[PALTUI_EXTRA];
    int saved;
} pst;

static int32_t pal_io_write(const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t off = 0;
    while (off < len) {
        int32_t r = pal_write(1, p + off, len - off);
        if (r <= 0) return -1;
        off += (uint32_t)r;
    }
    return (int32_t)len;
}
static int32_t pal_io_read(void *buf, uint32_t cap, int32_t timeout) {
    struct { int32_t h; uint16_t want, got; } r[2 + PALTUI_EXTRA];
    int idx[PALTUI_EXTRA], n = 0;
    if (!pst.init) { pst.ev = pal_events(); pst.init = 1; }
    if (pst.ev < 0) return TUI_IO_WAKE_QUIT;
    r[n].h = 0; r[n].want = PAL_POLL_IN; r[n].got = 0; n++;
    r[n].h = pst.ev; r[n].want = PAL_POLL_IN; r[n].got = 0; n++;
    for (int i = 0; i < PALTUI_EXTRA; i++) {
        idx[i] = -1;
        if (!pst.want[i]) continue;
        idx[i] = n;
        r[n].h = pst.h[i]; r[n].want = pst.want[i]; r[n].got = 0; n++;
        pst.got[i] = 0;
    }
    int32_t k = pal_wait(r, (uint32_t)n * 8, timeout);
    if (k < 0) return TUI_IO_WAKE_QUIT;
    if (k == 0) return 0;
    if (r[1].got) {
        uint32_t ev[4] = {0, 0, 0, 0};
        if (pal_event_next(ev, 16, 0) == 16) {
            if (ev[0] == PAL_EV_QUIT) return TUI_IO_WAKE_QUIT;
            if (ev[0] == PAL_EV_RESIZE) return TUI_IO_WAKE_RESIZE;
        }
    }
    if (r[0].got & PAL_POLL_IN) {
        int32_t m = pal_read(0, buf, cap);
        if (m > 0) return m;
        if (m == 0) return TUI_IO_WAKE_QUIT;
    }
    if (r[0].got & (PAL_POLL_HUP | PAL_POLL_ERR)) return TUI_IO_WAKE_QUIT;
    for (int i = 0; i < PALTUI_EXTRA; i++)
        if (idx[i] >= 0 && r[idx[i]].got) { pst.got[i] = r[idx[i]].got; return TUI_IO_WAKE_EXTRA; }
    return 0;
}
static int32_t pal_io_size(int *cols, int *rows) {
    uint8_t b[4];
    if (pal_tty_size(b, 4) < 0) return -1;
    *cols = b[0] | (b[1] << 8);
    *rows = b[2] | (b[3] << 8);
    return 0;
}
static int32_t pal_io_caps(void) {
    int32_t c = pal_tty_caps();
    return (c < 0 ? 0 : c) | TUI_CAP_16;
}
static int32_t pal_io_now(void) { return (int32_t)pal_now_ms(); }
static const tui_io_t pal_io = {pal_io_write, pal_io_read, pal_io_size, pal_io_caps, pal_io_now};
const tui_io_t *tui_io_pal(void) { return &pal_io; }

/* Raw mode while the screen is ours; the mode from before is restored afterwards. */
static void tty_mode(tui_t *t, int raw) {
    if (t->io.read != pal_io_read) return;
    if (raw) {
        int32_t m = pal_tty_get();
        pst.saved = m < 0 ? PAL_TTY_COOKED : (int)m;
        pal_tty_mode(PAL_TTY_RAW);
    } else pal_tty_mode(pst.saved);
}
int tui_wait_extra(tui_t *t, int32_t h, uint16_t want) {
    (void)t;
    int free_i = -1;
    for (int i = 0; i < PALTUI_EXTRA; i++) {
        if (pst.want[i] && pst.h[i] == h) { pst.want[i] = want; return 0; }
        if (!pst.want[i] && free_i < 0) free_i = i;
    }
    if (want == 0 || free_i < 0) return want == 0 ? 0 : -1;
    pst.h[free_i] = h;
    pst.want[free_i] = want;
    return 0;
}
uint16_t tui_extra_got(int32_t h) {
    for (int i = 0; i < PALTUI_EXTRA; i++)
        if (pst.want[i] && pst.h[i] == h) return pst.got[i];
    return 0;
}
#else
static void tty_mode(tui_t *t, int raw) { (void)t; (void)raw; }
#endif

