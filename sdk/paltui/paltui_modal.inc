/* ---- modal helpers ---- */
/* Centred frame for a modal; sets the region to its inside. */
static void box(tui_t *t, int w, int h, const char *title) {
    if (w > t->cols) w = t->cols;
    if (h > t->rows) h = t->rows;
    int x = (t->cols - w) / 2, y = (t->rows - h) / 2;
    frame(t, x, y, w, h, title);
    t->rx = x + 2;
    t->ry = y + 1;
    t->rw = w - 4;
    t->rh = h - 2;
    t->cy = t->ry;
}
typedef int (*modal_fn)(tui_t *t, void *ctx, int *result);
/* Runs a modal screen until its fn returns 1. The background is the last screen shown; each round it is copied
 * into the back buffer and the modal is drawn over it, so nothing of the modal stays behind. */
static int modal_run(tui_t *t, modal_fn fn, void *ctx, int dflt) {
    uint32_t saved = t->focus;
    int result = dflt, done = 0;
    t->focus = 0;
    rotate_ids(t);
    for (;;) {
        tcopy(t->cells[t->cur], t->cells[1 - t->cur], sizeof(t->cells[0]));
        done = fn(t, ctx, &result);
        tui_end(t);
        if (done) break;
        if (!begin(t, -1, 0)) { result = dflt; break; }
    }
    if (t->open) clear_back(t); /* the caller's frame goes on from an empty back buffer */
    t->focus = saved;
    return result;
}

typedef struct { const char *title; char *buf; uint32_t cap; } prompt_ctx;
static int prompt_fn(tui_t *t, void *c, int *res) {
    prompt_ctx *p = (prompt_ctx *)c;
    int w = t->cols * 2 / 3;
    if (w < 30) w = 30;
    if (w > t->cols - 2) w = t->cols - 2;
    box(t, w, 4, p->title);
    int r = field(t, "prompt.in", p->buf, p->cap, 0);
    static const char hint[] = "Enter: ok    Esc: cancel";
    if (can_draw(t)) text_put(t, t->rx, t->cy, hint, (int)tslen(hint), t->rw, TUI_DIM, 0);
    if (r & 2) { *res = 1; return 1; }
    const tui_key_t *k = peek_key(t);
    if (k && k->sym == TUI_K_ESC) { use_key(t); *res = 0; return 1; }
    return 0;
}
int tui_prompt(tui_t *t, const char *title, char *buf, uint32_t cap) {
    if (!t->open || cap == 0) return 0;
    prompt_ctx c = {title, buf, cap};
    return modal_run(t, prompt_fn, &c, 0);
}

typedef struct { const char *text; int yes; } confirm_ctx;
static int confirm_fn(tui_t *t, void *c, int *res) {
    confirm_ctx *q = (confirm_ctx *)c;
    int w = t->cols - 2;
    if (w > 50) w = 50;
    t->rw = w - 4;
    int h = wrap(t, q->text, 0) + 3;
    box(t, w, h, "Confirm");
    t->cy += wrap(t, q->text, 1);
    if (can_draw(t)) {
        int x = t->rx + text_put(t, t->rx, t->cy, "[ Yes ]", 7, t->rw, q->yes ? TUI_SEL : TUI_NORMAL, 0);
        text_put(t, x + 2, t->cy, "[ No ]", 6, t->rx + t->rw - x - 2, q->yes ? TUI_NORMAL : TUI_SEL, 0);
        t->cy++;
    }
    const tui_key_t *k = peek_key(t);
    if (!k) return 0;
    if (k->sym == TUI_K_ENTER) { use_key(t); *res = q->yes; return 1; }
    if (k->sym == TUI_K_ESC) { use_key(t); *res = 0; return 1; }
    if (k->sym == TUI_K_LEFT || k->sym == TUI_K_RIGHT) { use_key(t); q->yes = !q->yes; return 0; }
    if (k->sym == 0 && !k->mods && (k->cp == 'y' || k->cp == 'Y')) { use_key(t); *res = 1; return 1; }
    if (k->sym == 0 && !k->mods && (k->cp == 'n' || k->cp == 'N')) { use_key(t); *res = 0; return 1; }
    return 0;
}
int tui_confirm(tui_t *t, const char *text) {
    if (!t->open) return 0;
    confirm_ctx c = {text, 1};
    return modal_run(t, confirm_fn, &c, 0);
}

typedef struct { const char *title; const char *const *items; int n, sel; } pick_ctx;
static int pick_fn(tui_t *t, void *c, int *res) {
    pick_ctx *q = (pick_ctx *)c;
    int vis = q->n < 12 ? q->n : 12;
    if (vis > t->rows - 4) vis = t->rows - 4;
    if (vis < 1) vis = 1;
    int iw = 0;
    for (int i = 0; i < q->n; i++) {
        int w = str_w(q->items[i]) + 6;
        if (w > iw) iw = w;
    }
    int w = iw < 24 ? 24 : iw;
    if (w > t->cols - 2) w = t->cols - 2;
    box(t, w, vis + 2, q->title);
    if (list_run(t, "pick.list", q->items, q->n, &q->sel, 1)) { *res = q->sel; return 1; }
    const tui_key_t *k = peek_key(t);
    if (k && k->sym == TUI_K_ESC) { use_key(t); *res = -1; return 1; }
    return 0;
}
int tui_pick(tui_t *t, const char *title, const char *const *items, int n) {
    if (!t->open || n <= 0) return -1;
    pick_ctx c = {title, items, n, 0};
    return modal_run(t, pick_fn, &c, -1);
}

