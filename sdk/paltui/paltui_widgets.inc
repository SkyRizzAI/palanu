/* ---- layout and frame widgets ---- */
void tui_region(tui_t *t, int x, int y, int w, int h) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > t->cols) w = t->cols - x;
    if (y + h > t->rows) h = t->rows - y;
    t->rx = x;
    t->ry = y;
    t->rw = w < 0 ? 0 : w;
    t->rh = h < 0 ? 0 : h;
    t->cy = y;
}
void tui_gap(tui_t *t, int rows) { t->cy += rows; }
/* A bordered frame at absolute coordinates; the title sits in the top border. */
static void frame(tui_t *t, int x, int y, int w, int h, const char *title) {
    uint32_t hz = t->utf8 ? 0x2500 : '-', vt = t->utf8 ? 0x2502 : '|';
    uint32_t tl = t->utf8 ? 0x250C : '+', tr = t->utf8 ? 0x2510 : '+';
    uint32_t bl = t->utf8 ? 0x2514 : '+', br = t->utf8 ? 0x2518 : '+';
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t cp = ' ';
            int top = j == 0, bot = j == h - 1, lft = i == 0, rgt = i == w - 1;
            if (top || bot || lft || rgt) {
                cp = (top && lft) ? tl : (top && rgt) ? tr : (bot && lft) ? bl : (bot && rgt) ? br
                   : (top || bot) ? hz : vt;
                cell_abs(t, x + i, y + j, cp, TUI_DIM, 0);
            } else cell_abs(t, x + i, y + j, ' ', TUI_NORMAL, 0);
        }
    if (title && w > 4) text_put(t, x + 2, y, title, (int)tslen(title), w - 4, TUI_TITLE, 1);
}
void tui_box(tui_t *t, int x, int y, int w, int h, const char *title) {
    if (w >= 2 && h >= 2) frame(t, x, y, w, h, title);
}
void tui_status(tui_t *t, const char *text) {
    if (!t->open) return;
    for (int x = 0; x < t->cols; x++) cell_abs(t, x, t->rows - 1, ' ', TUI_SEL, 0);
    text_put(t, 1, t->rows - 1, text, (int)tslen(text), t->cols - 2, TUI_SEL, 1);
}
void tui_title(tui_t *t, const char *text) {
    if (!can_draw(t)) return;
    text_put(t, t->rx, t->cy, text, (int)tslen(text), t->rw, TUI_TITLE, 0);
    t->cy++;
}
void tui_label(tui_t *t, const char *text) {
    if (!can_draw(t)) return;
    text_put(t, t->rx, t->cy, text, (int)tslen(text), t->rw, TUI_NORMAL, 0);
    t->cy++;
}
/* Word wrap into the region's width. Draws the rows when draw is set; returns how many rows the text takes. */
static int wrap(tui_t *t, const char *s, int draw) {
    const char *end = s + tslen(s), *p = s;
    int rows = 0, w = t->rw;
    if (w < 1) return 0;
    for (;;) {
        const char *q = p, *sp = 0, *next, *line_end;
        int width = 0;
        while (q < end && *q != '\n') {
            int n;
            uint32_t cp = u8_dec(q, end, &n);
            int cw = cp_width(cp);
            if (width + cw > w) break;
            if (*q == ' ') sp = q;
            width += cw;
            q += n;
        }
        if (q >= end || *q == '\n') { line_end = q; next = q < end ? q + 1 : q; }
        else if (sp && sp > p) { line_end = sp; next = sp + 1; }
        else if (q > p) { line_end = q; next = q; }
        else { int n; u8_dec(q, end, &n); line_end = q + n; next = line_end; }
        if (draw && t->cy + rows < t->ry + t->rh)
            text_put(t, t->rx, t->cy + rows, p, (int)(line_end - p), w, TUI_NORMAL, 0);
        rows++;
        if (next >= end) break;
        p = next;
    }
    return rows;
}
void tui_text(tui_t *t, const char *text) {
    if (!can_draw(t)) return;
    t->cy += wrap(t, text, 1);
}

/* Shared list: a menu (activate: 1 when Enter picks) or a plain list (1 when the selection moved). */
static int list_run(tui_t *t, const char *id, const char *const *items, int n, int *sel, int activate) {
    int avail = t->ry + t->rh - t->cy, result = 0;
    if (t->rw <= 0 || avail <= 0) return 0;
    int f = reg_focus(t, id_hash(id));
    if (n <= 0) { *sel = 0; return 0; }
    tui_slot_t *s = slot(t, id_hash(id));
    if (*sel < 0) *sel = 0;
    if (*sel >= n) *sel = n - 1;
    int vis = avail < n ? avail : n, before = *sel;
    if (f) {
        const tui_key_t *k = peek_key(t);
        int nav = 1;
        if (!k) nav = 0;
        else if (k->sym == TUI_K_UP) *sel -= 1;
        else if (k->sym == TUI_K_DOWN) *sel += 1;
        else if (k->sym == TUI_K_HOME) *sel = 0;
        else if (k->sym == TUI_K_END) *sel = n - 1;
        else if (k->sym == TUI_K_PGUP) *sel -= vis;
        else if (k->sym == TUI_K_PGDN) *sel += vis;
        else if (k->sym == TUI_K_ENTER && activate) result = 1;
        else nav = 0;
        if (nav || result) use_key(t);
    }
    if (*sel < 0) *sel = 0;
    if (*sel >= n) *sel = n - 1;
    if ((int)s->scroll > *sel) s->scroll = (uint32_t)*sel;
    if (*sel >= (int)s->scroll + vis) s->scroll = (uint32_t)(*sel - vis + 1);
    if ((int)s->scroll > n - vis) s->scroll = (uint32_t)(n - vis);
    int first = (int)s->scroll, hw = 0;
    for (int i = 0; i < n; i++) {
        int w = str_w(items[i]) + 2;
        if (w > hw) hw = w;
    }
    if (hw > t->rw) hw = t->rw;
    for (int r = 0; r < vis; r++) {
        int i = first + r, y = t->cy + r, on = i == *sel;
        int style = on ? (f ? TUI_SEL : TUI_ACCENT) : TUI_NORMAL;
        if (on) fill(t, t->rx, y, hw, style);
        int x = t->rx + text_put(t, t->rx, y, on ? "> " : "  ", 2, t->rw, style, 0);
        text_put(t, x, y, items[i], (int)tslen(items[i]), t->rx + t->rw - x, style, 0);
    }
    t->cy += vis;
    return activate ? result : (*sel != before);
}
int tui_menu(tui_t *t, const char *id, const char *const *items, int n, int *sel) {
    return list_run(t, id, items, n, sel, 1);
}
int tui_list(tui_t *t, const char *id, const char *const *items, int n, int *sel) {
    return list_run(t, id, items, n, sel, 0);
}

static int ins(char *buf, uint32_t cap, uint32_t *len, uint32_t *pos, const char *s, uint32_t n) {
    if (*len + n >= cap) return 0;
    tmove((uint8_t *)buf + *pos + n, (const uint8_t *)buf + *pos, *len - *pos + 1);
    tcopy(buf + *pos, s, n);
    *len += n;
    *pos += n;
    return 1;
}
static uint32_t cp_prev(const char *b, uint32_t p) {
    if (p == 0) return 0;
    p--;
    while (p > 0 && (b[p] & 0xC0) == 0x80) p--;
    return p;
}
static uint32_t cp_next(const char *b, uint32_t len, uint32_t p) {
    if (p >= len) return len;
    p++;
    while (p < len && (b[p] & 0xC0) == 0x80) p++;
    return p;
}
/* Display width of buf[a..b): one column per character when secret (masked). */
static int span_w(const char *a, const char *b, int secret) {
    int w = 0, n;
    while (a < b) {
        uint32_t cp = u8_dec(a, b, &n);
        w += secret ? 1 : cp_width(cp);
        a += n;
    }
    return w;
}
/* The line editor shared by tui_input and tui_password. buf is NUL-terminated within cap. */
static int field(tui_t *t, const char *id, char *buf, uint32_t cap, int secret) {
    if (!can_draw(t) || cap == 0) return 0;
    uint32_t h = id_hash(id);
    int f = reg_focus(t, h), ret = 0;
    tui_slot_t *s = slot(t, h);
    uint32_t len = tslen(buf);
    if (len >= cap) { len = cap - 1; buf[len] = 0; }
    uint32_t pos = s->pos > len ? len : s->pos;
    if (f) {
        const tui_key_t *k = peek_key(t);
        const tui_key_t none = {0, 0, 0};
        if (!k) k = &none;
        int ctrl = (k->mods & TUI_MOD_CTRL) != 0;
        if (k->sym == 0 && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT)) && k->cp >= 0x20 && k->cp != 0x7f) {
            char e[4];
            if (ins(buf, cap, &len, &pos, e, (uint32_t)u8_enc(k->cp, e))) ret |= 1;
            use_key(t);
        } else if (k->sym == TUI_K_ENTER) { ret |= 2; use_key(t); }
        else if (k->sym == TUI_K_BACKSPACE && pos > 0) {
            uint32_t p0 = cp_prev(buf, pos);
            tmove((uint8_t *)buf + p0, (const uint8_t *)buf + pos, len - pos + 1);
            len -= pos - p0;
            pos = p0;
            ret |= 1;
            use_key(t);
        } else if (k->sym == TUI_K_DELETE && pos < len) {
            uint32_t p1 = cp_next(buf, len, pos);
            tmove((uint8_t *)buf + pos, (const uint8_t *)buf + p1, len - p1 + 1);
            len -= p1 - pos;
            ret |= 1;
            use_key(t);
        } else if (k->sym == TUI_K_LEFT) { pos = cp_prev(buf, pos); use_key(t); }
        else if (k->sym == TUI_K_RIGHT) { pos = cp_next(buf, len, pos); use_key(t); }
        else if (k->sym == TUI_K_HOME || (ctrl && k->cp == 'a')) { pos = 0; use_key(t); }
        else if (k->sym == TUI_K_END || (ctrl && k->cp == 'e')) { pos = len; use_key(t); }
        else if (ctrl && k->cp == 'u' && pos > 0) {
            tmove((uint8_t *)buf, (const uint8_t *)buf + pos, len - pos + 1);
            len -= pos;
            pos = 0;
            ret |= 1;
            use_key(t);
        }
        if (t->ev == EV_PASTE && !t->consumed) {
            /* paste: printable characters only, tabs become spaces */
            const char *e = t->paste, *pe = t->paste + t->paste_len;
            while (e < pe) {
                int n;
                uint32_t cp = u8_dec(e, pe, &n);
                e += n;
                if (cp == '\t') cp = ' ';
                if (cp < 0x20 || cp == 0x7f) continue;
                char enc[4];
                if (ins(buf, cap, &len, &pos, enc, (uint32_t)u8_enc(cp, enc))) ret |= 1;
            }
            use_key(t);
        }
    }
    /* keep the cursor on screen: the first shown byte moves right until the cursor fits */
    uint32_t start = s->scroll > pos ? pos : s->scroll;
    while (start < pos && span_w(buf + start, buf + pos, secret) >= t->rw) start = cp_next(buf, len, start);
    s->scroll = start;
    s->pos = pos;
    int y = t->cy, x = t->rx, col = 0;
    fill(t, x, y, t->rw, TUI_INPUT);
    for (uint32_t i = start; i < len;) {
        int n;
        uint32_t cp = u8_dec(buf + i, buf + len, &n);
        int cw = secret ? 1 : cp_width(cp);
        if (col + cw > t->rw) break;
        draw_cp(t, x + col, y, secret ? '*' : cp, TUI_INPUT, 0);
        col += cw;
        i += n;
    }
    if (f) {
        int cc = span_w(buf + start, buf + pos, secret);
        if (cc < t->rw) bcell(t, x + cc, y)->attr |= TUI_A_CURSOR;
    }
    t->cy++;
    return ret;
}
int tui_input(tui_t *t, const char *id, char *buf, uint32_t cap) { return field(t, id, buf, cap, 0); }
int tui_password(tui_t *t, const char *id, char *buf, uint32_t cap) { return field(t, id, buf, cap, 1); }

int tui_button(tui_t *t, const char *id, const char *label) {
    if (!can_draw(t)) return 0;
    int f = reg_focus(t, id_hash(id)), r = 0;
    const tui_key_t *k = peek_key(t);
    if (f && k && (k->sym == TUI_K_ENTER || (k->sym == 0 && k->cp == ' ' && !k->mods))) { use_key(t); r = 1; }
    int style = f ? TUI_SEL : TUI_NORMAL, w = (int)tslen(label) + 4;
    if (w > t->rw) w = t->rw;
    fill(t, t->rx, t->cy, w, style);
    int x = t->rx + text_put(t, t->rx, t->cy, "[ ", 2, t->rw, style, 0);
    x += text_put(t, x, t->cy, label, (int)tslen(label), t->rx + t->rw - x - 2, style, 0);
    text_put(t, x, t->cy, " ]", 2, t->rx + t->rw - x, style, 0);
    t->cy++;
    return r;
}
int tui_checkbox(tui_t *t, const char *id, const char *label, int *on) {
    if (!can_draw(t)) return 0;
    int f = reg_focus(t, id_hash(id)), r = 0;
    const tui_key_t *k = peek_key(t);
    if (f && k && (k->sym == TUI_K_ENTER || (k->sym == 0 && k->cp == ' ' && !k->mods))) {
        use_key(t);
        *on = !*on;
        r = 1;
    }
    int style = f ? TUI_SEL : TUI_NORMAL;
    int x = t->rx + text_put(t, t->rx, t->cy, *on ? "[x] " : "[ ] ", 4, t->rw, style, 0);
    text_put(t, x, t->cy, label, (int)tslen(label), t->rx + t->rw - x, style, 0);
    t->cy++;
    return r;
}
void tui_progress(tui_t *t, uint32_t value, uint32_t max) {
    if (!can_draw(t)) return;
    if (max == 0) max = 1;
    if (value > max) value = max;
    int bw = t->rw - 7;
    if (bw < 1) return;
    uint32_t filled = (uint32_t)((uint64_t)value * (uint32_t)bw / max);
    int y = t->cy;
    draw_cp(t, t->rx, y, '[', TUI_NORMAL, 0);
    for (int i = 0; i < bw; i++)
        draw_cp(t, t->rx + 1 + i, y, (uint32_t)i < filled ? '#' : '-', (uint32_t)i < filled ? TUI_OK : TUI_DIM, 0);
    draw_cp(t, t->rx + 1 + bw, y, ']', TUI_NORMAL, 0);
    char b[16];
    b[0] = ' ';
    int n = 1 + num_str(b + 1, (uint32_t)((uint64_t)value * 100 / max));
    b[n++] = '%';
    text_put(t, t->rx + 2 + bw, y, b, n, t->rw - 2 - bw, TUI_NORMAL, 0);
    t->cy++;
}

