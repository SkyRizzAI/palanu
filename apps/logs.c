/* The Logs app (view "logs"), drawn as the reference logs screen: the header
 * "Logs (N)", three toolbar rows (Follow newest, Level, Clear logs), then the lines in the mono font as "[I] comp: message", cut
 * at the right edge. Up and Down move the toolbar focus; past its ends they scroll the lines. Enter acts on the focused row, Left
 * and Right change the level. The nema log ring and the service logs (psys_svc_log_read) are merged in time order (logs_model.c).
 * Tab opens the options menu (follow, level, source, clear view). "/" searches the lines, n and N go on. The log is only read
 * while the app is in front (a 500 ms tick), so the view costs nothing in the background. The screen is also bound as a document
 * (pal/doc.h): the classic session draws it with draw below, a session that draws documents (Athena) in its own look. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "libapp.h"
#include "logs_model.h"
#include "nema/log.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/doc.h"
#include "pal/kui.h"
#include "pal/svc.h"
#include "pal/sysinfo.h"
#include "pal/toast.h"
#include "pal/vk.h"
#include "views.h"

#define RING_CAP (16u * 1024u + 1u) /* the ring is 16 KB; the snapshot adds the terminating byte */
#define SVC_CAP (8u * 1024u)        /* service lines kept, oldest dropped */
#define SVC_MAX 16
#define TICK_REFRESH_MS 500
#define MAX_COMPS 12
#define TOOL_PAD 2  /* the toolbar's inset from the header and the sides (the reference: padding 2, gap 2) */
#define BODY_LINE 9 /* a line of the mono body: the 8 px cell and a 1 px gap */
#define BAR_X (k->w - 3) /* the scrollbar's x (logical): the 3 px bar at the right edge, at any UI scale */
#define TOOL_ROWS 3

enum { M_FOLLOW, M_LEVEL, M_COMP, M_CLEAR };

static struct {
    char *ring;
    size_t ring_len;
    uint32_t ring_hash;
    char *svc;
    size_t svc_len;
    logs_ref_t *refs;
    int n;
    int *vis;
    int nvis;
    int filter;
    char comp[LOGS_COMP];
    bool follow;
    int top, page;
    char marker[200];          /* the newest line when the view was cleared; lines up to it are hidden */
    struct {
        char id[24];
        uint64_t cur;
    } cur[SVC_MAX];
    int ncur;
    bool owned, dirty;
    bool want;                 /* pal_logs_open waiting for the pump */
    char want_comp[LOGS_COMP];
    char query[40];
    /* menus: the labels and the component names must stay valid while their picker is open */
    char mtext[4][40];
    int mact[4];
    const char *mitems[PAL_PICKER_ITEMS];
    int mn;
    char comps[MAX_COMPS + 1][LOGS_COMP];
    const char *citems[MAX_COMPS + 1];
    int cn;
    pal_vk_entry_t ve;
    char vebuf[40];
    int focus;                 /* the toolbar row that has the focus: 0 follow, 1 level, 2 clear */
    kui_canvas_t kc;
} L NEMA_EXT_BSS;

static pal_doc_t DOC; /* the screen as a document (below) */

static bool alloc_all(void) {
    if (L.owned) return true;
    L.ring = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, RING_CAP);
    L.svc = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, SVC_CAP);
    L.refs = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, LOGS_MAX * sizeof(logs_ref_t));
    L.vis = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, LOGS_MAX * sizeof(int));
    if (!L.ring || !L.svc || !L.refs || !L.vis) {
        nema_free(L.ring);
        nema_free(L.svc);
        nema_free(L.refs);
        nema_free(L.vis);
        L.ring = L.svc = NULL;
        L.refs = NULL;
        L.vis = NULL;
        return false;
    }
    L.owned = true;
    L.follow = true; /* a new view starts on the newest line */
    return true;
}

static uint32_t hash_bytes(const char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h ^ (uint32_t)n;
}

static uint32_t uptime_ms(void) { return (uint32_t)(pal_sys_uptime_us() / 1000u); }

/* Appends service text; when the buffer is full, the oldest whole lines go. */
static void svc_append(const char *s, size_t n) {
    if (n >= SVC_CAP) {
        s += n - (SVC_CAP / 2);
        n = SVC_CAP / 2;
    }
    if (L.svc_len + n > SVC_CAP) {
        size_t drop = L.svc_len + n - SVC_CAP;
        while (drop < L.svc_len && L.svc[drop] != '\n') drop++;
        if (drop < L.svc_len) drop++;
        memmove(L.svc, L.svc + drop, L.svc_len - drop);
        L.svc_len -= drop;
    }
    memcpy(L.svc + L.svc_len, s, n);
    L.svc_len += n;
}

/* Reads the new lines of every service into the service buffer. Returns true when something arrived. */
static bool pull_services(void) {
    bool any = false;
    size_t count = psys_svc_count();
    for (size_t i = 0; i < count && i < PSYS_SVC_MAX; i++) {
        psys_svc_info_t info;
        if (!psys_svc_info(i, &info)) continue;
        int slot = -1;
        for (int k = 0; k < L.ncur && slot < 0; k++)
            if (strcmp(L.cur[k].id, info.id) == 0) slot = k;
        if (slot < 0 && L.ncur < SVC_MAX) {
            slot = L.ncur++;
            snprintf(L.cur[slot].id, sizeof(L.cur[slot].id), "%s", info.id);
            L.cur[slot].cur = 0;
        }
        if (slot < 0) continue;
        char buf[256];
        for (;;) {
            int n = psys_svc_log_read(info.id, &L.cur[slot].cur, buf, sizeof(buf));
            if (n <= 0) break;
            any = true;
            /* one service line per log line: each newline in the chunk starts its own entry */
            size_t p = 0;
            while (p < (size_t)n) {
                size_t e = p;
                while (e < (size_t)n && buf[e] != '\n' && buf[e] != '\r') e++;
                if (e > p) {
                    char line[200];
                    int w = snprintf(line, sizeof(line), "I (%lu) %s: %.*s\n", (unsigned long)uptime_ms(), info.id, (int)(e - p),
                                     buf + p);
                    if (w > 0) svc_append(line, (size_t)w < sizeof(line) ? (size_t)w : sizeof(line) - 1);
                }
                p = e + 1;
            }
        }
    }
    return any;
}

static void rebuild(void) {
    int from = (int)logs_after_marker(L.ring, L.ring_len, L.marker);
    L.n = logs_add_lines(L.refs, 0, LOGS_MAX, L.ring, L.ring_len, (size_t)from, LOGS_SRC_RING);
    L.n = logs_add_lines(L.refs, L.n, LOGS_MAX, L.svc, L.svc_len, 0, LOGS_SRC_SVC);
    logs_sort(L.refs, L.n);
    L.nvis = logs_visible(L.refs, L.n, L.filter, L.comp, L.vis, LOGS_MAX);
    if (L.follow) L.top = logs_follow_top(L.nvis, L.page);
    L.dirty = false;
    DOC.gen++;
}

static void refresh(void) {
    size_t n = nema_log_snapshot(L.ring, RING_CAP);
    L.ring_len = n;
    uint32_t h = hash_bytes(L.ring, n);
    bool svc = pull_services();
    if (h != L.ring_hash || svc || L.dirty) {
        L.ring_hash = h;
        rebuild();
        pal_desktop_invalidate();
    }
}

/* ---- drawing ---- */
/* The line as shown: the reference "[I] comp: message" (the message follows "tag: " in the stored line). */
static void format_row(char *out, size_t cap, const logs_ref_t *r) {
    static const char LETTER[] = "EWID";
    const char *text = r->src == LOGS_SRC_RING ? L.ring + r->off : L.svc + r->off;
    const char *msg = text;
    size_t msglen = r->len;
    for (size_t i = 0; i + 1 < r->len; i++)
        if (text[i] == ':' && text[i + 1] == ' ') {
            msg = text + i + 2;
            msglen = r->len - (size_t)(i + 2);
            break;
        }
    snprintf(out, cap, "[%c] %s: %.*s", LETTER[r->level & 3], r->comp, (int)msglen, msg);
}

static bool row_has(const logs_ref_t *r, const char *q) {
    if (!q[0]) return false;
    char line[200];
    format_row(line, sizeof(line), r);
    size_t m = strlen(q);
    for (size_t i = 0; line[i]; i++) {
        size_t k = 0;
        while (k < m && line[i + k] && ((line[i + k] | 32) == (q[k] | 32))) k++;
        if (k == m) return true;
    }
    return false;
}

static void draw_toolbar(kui_canvas_t *k, int y, int w) {
    const int h = KUI_LIST_ROW_H;
    int x = TOOL_PAD;
    kui_row_label(k, x, y, h, w - 2 * 5 - 18 - 2, "Follow newest");
    kui_row_switch(k, x, y, h, w, L.follow);
    kui_row_frame(k, x, y, w, h, L.focus == 0);
    y += KUI_LIST_PITCH;
    kui_list_choice(k, x, y, w, "Level", logs_filter_name(L.filter), true, true);
    kui_row_frame(k, x, y, w, h, L.focus == 1);
    if (L.n > 0) {
        y += KUI_LIST_PITCH;
        kui_list_item_t it = {0};
        it.label = "Clear logs";
        it.icon = -1;
        it.selected = L.focus == 2;
        kui_list_item(k, x, y, w, &it);
    }
}

static int toolbar_rows(void) { return L.n > 0 ? TOOL_ROWS : TOOL_ROWS - 1; }

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&L.kc, c->w, c->h, kui_scale_q());
    kui_canvas_t *k = &L.kc;
    int ay = kui_lg(k, a.y), ah = kui_lg(k, a.y + a.h) - ay;
    kui_clip(k, 0, ay, k->w, ah);
    if (L.focus >= toolbar_rows()) L.focus = toolbar_rows() - 1;
    char title[40];
    snprintf(title, sizeof(title), "Logs (%d)", L.n);
    int y = ay + TOOL_PAD + kui_section(k, TOOL_PAD, ay + TOOL_PAD, &FONT_BOLD8, title);
    int tw = k->w - 2 * TOOL_PAD;
    draw_toolbar(k, y + TOOL_PAD, tw);
    int body = y + TOOL_PAD + toolbar_rows() * KUI_LIST_PITCH - 2 + 2; /* the rows, then the gap that leads to the lines */
    int bottom = ay + ah;
    int vis = (bottom - body) / BODY_LINE;
    if (vis < 1) vis = 1;
    L.page = vis;
    if (L.follow) L.top = logs_follow_top(L.nvis, vis);
    int max = L.nvis - vis;
    if (L.top > max) L.top = max < 0 ? 0 : max;
    if (L.top < 0) L.top = 0;
    kui_clip(k, 0, body, k->w, bottom - body);
    if (L.nvis == 0) {
        const char *msg = L.n == 0 ? "No logs yet" : "No lines match the filter";
        kui_text(k, &FONT_6X8, 2, body, msg, true);
    }
    for (int i = 0; i < vis && L.top + i < L.nvis; i++) {
        const logs_ref_t *r = &L.refs[L.vis[L.top + i]];
        char line[200];
        format_row(line, sizeof(line), r);
        int ly = body + i * BODY_LINE;
        kui_text(k, &FONT_6X8, 2, ly, line, true);
        if (row_has(r, L.query)) kui_invert(k, 0, ly, k->w - 4, FONT_6X8.charH);
    }
    kui_clip_reset(k);
    kui_clip(k, 0, ay, k->w, ah);
    if (L.nvis > vis) kui_scrollbar(k, BAR_X, body, vis * BODY_LINE, L.top * BODY_LINE, vis * BODY_LINE, L.nvis * BODY_LINE);
    kui_present(c, k, a);
}

/* ---- the document (pal/doc.h): the same screen as data, for a session that draws it in its own look ---- */
static void doc_title(void *ctx, char *out, size_t cap) { (void)ctx, snprintf(out, cap, "Logs (%d)", L.n); }
static int doc_follow(void *ctx) { return (void)ctx, L.follow; }
static void doc_level(void *ctx, char *out, size_t cap) { (void)ctx, snprintf(out, cap, "%s", logs_filter_name(L.filter)); }
static const pal_doc_tool_t DOC_TOOLS[] = {
    {.label = "Follow newest", .get = doc_follow},
    {.label = "Level", .value = doc_level},
    {.label = "Clear logs"},
};
static int doc_ntools(void *ctx) { return (void)ctx, toolbar_rows(); }
static int doc_count(void *ctx) { return (void)ctx, L.nvis; }
static void doc_line(void *ctx, int i, char *out, size_t cap, bool *mark) {
    (void)ctx;
    if (i < 0 || i >= L.nvis) {
        out[0] = 0;
        return;
    }
    const logs_ref_t *r = &L.refs[L.vis[i]];
    format_row(out, cap, r);
    *mark = row_has(r, L.query);
}
static void doc_empty(void *ctx, char *out, size_t cap) {
    (void)ctx, snprintf(out, cap, "%s", L.n == 0 ? "No logs yet" : "No lines match the filter");
}
static pal_doc_t DOC = {.title = doc_title, .tools = DOC_TOOLS, .ntools = doc_ntools, .focus = &L.focus, .count = doc_count,
                        .line = doc_line, .empty = doc_empty, .top = &L.top, .vis = &L.page, .mono = false};

/* ---- search ---- */
static void find(bool forward) {
    if (!L.query[0]) { psys_toast("No search text", 1500); return; }
    int n = L.nvis;
    if (n == 0) { psys_toast("Not found", 1500); return; }
    for (int step = 1; step <= n; step++) {
        int i = ((L.top + (forward ? step : -step)) % n + n) % n;
        if (row_has(&L.refs[L.vis[i]], L.query)) {
            L.follow = false;
            L.top = i > 1 ? i - 1 : 0;
            pal_desktop_invalidate();
            return;
        }
    }
    psys_toast("Not found", 1500);
}

static void on_find(int len, void *ctx) {
    (void)ctx;
    if (len < 0) return;
    find(true);
}

static void open_find(void) {
    memset(&L.ve, 0, sizeof(L.ve));
    snprintf(L.vebuf, sizeof(L.vebuf), "%s", L.query);
    L.ve.title = "Find";
    L.ve.initial = L.vebuf;
    L.ve.buf = L.vebuf;
    L.ve.cap = sizeof(L.vebuf);
    L.ve.allow_empty = false;
    if (pal_vk_edit_async(&L.ve, on_find, NULL) != NEMA_OK) { psys_toast("Keyboard is busy", 1500); return; }
    snprintf(L.query, sizeof(L.query), "%s", L.vebuf);
}

/* ---- the options menu (Tab) ---- */
/* Hides the lines the ring holds now: the view remembers the newest line and shows what comes after it. The ring has no clear call. */
static void clear_now(void) {
    L.marker[0] = 0;
    if (L.ring_len) {
        size_t end = L.ring_len;
        if (L.ring[end - 1] == '\n') end--;
        size_t start = end;
        while (start > 0 && L.ring[start - 1] != '\n') start--;
        size_t len = end - start;
        if (len >= sizeof(L.marker)) len = sizeof(L.marker) - 1;
        memcpy(L.marker, L.ring + start, len);
        L.marker[len] = 0;
    }
    L.svc_len = 0;
    L.top = 0;
    L.dirty = true;
    rebuild();
    pal_desktop_invalidate();
}

static void on_clear(bool ok, void *ctx) {
    (void)ctx;
    if (ok) clear_now();
}

void pal_logs_mark_clear(void) {
    if (!alloc_all()) return;
    L.ring_len = nema_log_snapshot(L.ring, RING_CAP);
    clear_now();
}

static void on_level(int idx, void *ctx) {
    (void)ctx;
    if (idx < 0) return;
    L.filter = idx;
    L.dirty = true;
    rebuild();
    pal_desktop_invalidate();
}

static void on_comp(int idx, void *ctx) {
    (void)ctx;
    if (idx < 0) return;
    if (idx == 0) L.comp[0] = 0;
    else snprintf(L.comp, sizeof(L.comp), "%s", L.comps[idx]);
    L.dirty = true;
    rebuild();
    pal_desktop_invalidate();
}

static void open_comp(void) {
    L.cn = 0;
    L.citems[L.cn] = "All";
    snprintf(L.comps[L.cn], LOGS_COMP, "All");
    L.cn++;
    char names[MAX_COMPS][LOGS_COMP];
    int k = logs_components(L.refs, L.n, names, MAX_COMPS);
    for (int i = 0; i < k && L.cn <= MAX_COMPS; i++) {
        snprintf(L.comps[L.cn], LOGS_COMP, "%s", names[i]);
        L.citems[L.cn] = L.comps[L.cn];
        L.cn++;
    }
    pal_picker_open("Source", L.citems, L.cn, on_comp, NULL);
}

static void open_level(void) {
    static const char *const LEVELS[LOGS_F_COUNT] = {"All", "Info+", "Warn+", "Error"};
    pal_picker_open("Level", LEVELS, LOGS_F_COUNT, on_level, NULL);
}

static void on_menu(int idx, void *ctx) {
    (void)ctx;
    if (idx < 0 || idx >= L.mn) return;
    switch (L.mact[idx]) {
    case M_FOLLOW:
        L.follow = !L.follow;
        if (L.follow) L.top = logs_follow_top(L.nvis, L.page);
        pal_desktop_invalidate();
        break;
    case M_LEVEL: open_level(); break;
    case M_COMP: open_comp(); break;
    case M_CLEAR: pal_dialog_confirm("Clear logs?", "Hide the lines shown now. The log itself is kept.", "Clear", "Cancel", on_clear, NULL); break;
    default: break;
    }
}

static void open_menu(void) {
    L.mn = 0;
    snprintf(L.mtext[L.mn], sizeof(L.mtext[0]), "Follow newest: %s", L.follow ? "on" : "off");
    L.mact[L.mn] = M_FOLLOW;
    L.mitems[L.mn] = L.mtext[L.mn];
    L.mn++;
    snprintf(L.mtext[L.mn], sizeof(L.mtext[0]), "Level: %s", logs_filter_name(L.filter));
    L.mact[L.mn] = M_LEVEL;
    L.mitems[L.mn] = L.mtext[L.mn];
    L.mn++;
    snprintf(L.mtext[L.mn], sizeof(L.mtext[0]), "Source: %s", L.comp[0] ? L.comp : "all");
    L.mact[L.mn] = M_COMP;
    L.mitems[L.mn] = L.mtext[L.mn];
    L.mn++;
    snprintf(L.mtext[L.mn], sizeof(L.mtext[0]), "Clear view");
    L.mact[L.mn] = M_CLEAR;
    L.mitems[L.mn] = L.mtext[L.mn];
    L.mn++;
    pal_picker_open("Logs", L.mitems, L.mn, on_menu, NULL);
}

/* ---- keys, time, lifecycle ---- */
/* Enter on the focused toolbar row. */
static void activate_row(void) {
    switch (L.focus) {
    case 0:
        L.follow = !L.follow;
        if (L.follow) L.top = logs_follow_top(L.nvis, L.page);
        break;
    case 1:
        L.filter = (L.filter + 1) % LOGS_F_COUNT;
        L.dirty = true;
        rebuild();
        break;
    case 2: pal_dialog_confirm("Clear logs?", "Hide the lines shown now. The log itself is kept.", "Clear", "Cancel", on_clear, NULL); break;
    default: break;
    }
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    int max = L.nvis - L.page;
    if (max < 0) max = 0;
    switch (k->sym) {
    case PAL_KEY_UP:
        if (L.focus > 0) L.focus--;
        else { L.follow = false; if (L.top > 0) L.top--; }
        break;
    case PAL_KEY_DOWN:
        if (L.focus + 1 < toolbar_rows()) L.focus++;
        else { L.follow = false; if (L.top < max) L.top++; }
        break;
    case PAL_KEY_ENTER: activate_row(); break;
    case PAL_KEY_LEFT:
        if (L.focus != 1) return true;
        L.filter = (L.filter + LOGS_F_COUNT - 1) % LOGS_F_COUNT;
        L.dirty = true;
        rebuild();
        break;
    case PAL_KEY_RIGHT:
        if (L.focus != 1) return true;
        L.filter = (L.filter + 1) % LOGS_F_COUNT;
        L.dirty = true;
        rebuild();
        break;
    case PAL_KEY_PGUP: L.follow = false; L.top -= L.page > 1 ? L.page - 1 : 1; if (L.top < 0) L.top = 0; break;
    case PAL_KEY_PGDN: L.follow = false; L.top += L.page > 1 ? L.page - 1 : 1; if (L.top > max) L.top = max; break;
    case PAL_KEY_HOME: L.follow = false; L.top = 0; break;
    case PAL_KEY_END: L.follow = true; L.top = max; break;
    case PAL_KEY_TAB: open_menu(); return true;
    case PAL_KEY_ESC: return false;
    case PAL_KEY_CHAR:
        if (k->cp == '/') open_find();
        else if (k->cp == 'n') find(true);
        else if (k->cp == 'N') find(false);
        else return true;
        break;
    default: return true;
    }
    if (L.top > max) L.top = max; /* the page can be another look's (pal/doc.h) */
    if (L.top < 0) L.top = 0;
    pal_desktop_invalidate();
    return true;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    refresh();
}

static void pump(pal_view_t *v) {
    (void)v;
    if (!L.want) return;
    L.want = false;
    L.comp[0] = 0;
    snprintf(L.comp, sizeof(L.comp), "%s", L.want_comp);
    L.follow = true;
    L.dirty = true;
    refresh();
}

static void enter(pal_view_t *v) {
    (void)v;
    if (!alloc_all()) {
        psys_toast("Out of memory", 1500);
        return;
    }
    if (!L.page) L.page = 8; /* until a look has drawn the lines */
    pal_doc_bind("logs", &DOC);
    refresh();
}

void pal_logs_open(const char *component) {
    snprintf(L.want_comp, sizeof(L.want_comp), "%s", component ? component : "");
    L.want = true;
    pal_desktop_open("logs");
}

pal_view_t pal_logs_view = {
    .id = "logs",
    .title = "Logs",
    .draw = draw,
    .key = key,
    .enter = enter,
    .pump = pump,
    .tick = tick,
    .tick_ms = TICK_REFRESH_MS,
};
