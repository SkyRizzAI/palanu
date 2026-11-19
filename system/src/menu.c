/* PalanuOS menu pages: the stack, the keys, and the asking calls (pal/menu.h). Drawing is in menu_draw.c. Rows are data;
 * the functions here only read them and call their callbacks. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"

#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/desktops.h"
#include "pal/menu.h"
#include "pal/settings_schema.h"
#include "pal/nav.h"
#include "pal/overlay.h"
#include "pal/toast.h"
#include "pal/vk.h"

static pal_menu_hooks_t s_hooks; /* the desktop defaults, unless a test replaced them */
static bool s_hooks_set;

/* The virtual keyboard keeps the entry struct and its buffers until the desktop takes them (vk.c stores the pointers),
 * so they are static here: one text question is open at a time. */
static pal_vk_entry_t s_vk;
static char s_vk_buf[PAL_MENU_TEXT_MAX];
static char s_vk_init[PAL_MENU_TEXT_MAX];

/* ---- desktop defaults ---- */

static void on_pick(int index, void *ctx) { pal_menu_answer_pick((pal_menu_t *)ctx, index); }
static void on_confirm(bool ok, void *ctx) { pal_menu_answer_confirm((pal_menu_t *)ctx, ok); }
static void on_vk(int len, void *ctx) {
    pal_menu_t *m = (pal_menu_t *)ctx;
    if (len < 0) pal_menu_answer_ask(m, false, NULL);
    else pal_menu_answer_ask(m, true, s_vk_buf);
}

static void d_picker(pal_menu_t *m, const char *title, const char *const *items, int n) {
    if (pal_picker_open(title, items, n, on_pick, m) != NEMA_OK) pal_menu_answer_pick(m, -1);
}

static void d_confirm(pal_menu_t *m, const char *title, const char *text, const char *ok) {
    if (pal_dialog_confirm(title, text, ok, NULL, on_confirm, m) != NEMA_OK) pal_menu_answer_confirm(m, false);
}

static void d_ask(pal_menu_t *m, const char *title, const char *initial, int max_len, bool mask) {
    size_t cap = PAL_MENU_TEXT_MAX;
    if (max_len > 0 && (size_t)max_len + 1 < cap) cap = (size_t)max_len + 1;
    snprintf(s_vk_init, sizeof(s_vk_init), "%s", initial ? initial : "");
    s_vk_buf[0] = 0;
    s_vk = (pal_vk_entry_t){.title = title, .initial = s_vk_init, .buf = s_vk_buf, .cap = cap, .mask = mask,
                            .allow_empty = true};
    if (pal_vk_edit_async(&s_vk, on_vk, m) != NEMA_OK) pal_menu_answer_ask(m, false, NULL);
}

static void d_toast(const char *text) { (void)psys_toast(text, 0); }

void pal_menu_set_hooks(const pal_menu_hooks_t *h) {
    if (!h) {
        memset(&s_hooks, 0, sizeof(s_hooks));
        s_hooks_set = false;
        return;
    }
    s_hooks = *h;
    s_hooks_set = true;
}

static void ask_picker(pal_menu_t *m, const char *t, const char *const *i, int n) {
    if (s_hooks_set && s_hooks.picker) s_hooks.picker(m, t, i, n);
    else d_picker(m, t, i, n);
}
static void ask_confirm(pal_menu_t *m, const char *t, const char *x, const char *ok) {
    if (s_hooks_set && s_hooks.confirm) s_hooks.confirm(m, t, x, ok);
    else d_confirm(m, t, x, ok);
}
static void ask_text(pal_menu_t *m, const char *t, const char *init, int max_len, bool mask) {
    if (s_hooks_set && s_hooks.ask) s_hooks.ask(m, t, init, max_len, mask);
    else d_ask(m, t, init, max_len, mask);
}
void pal_menu_toast(const char *text) {
    if (s_hooks_set && s_hooks.toast) s_hooks.toast(text);
    else d_toast(text);
}

/* ---- pages and the stack ---- */

/* A read-only gauge (a SLIDER with no setter) is not focusable, as the reference progress bar is not. */
bool pal_menu_gauge(const pal_menu_row_t *r) { return r && r->kind == PAL_MENU_SLIDER && !r->set; }

bool pal_menu_selectable(const pal_menu_row_t *r) {
    return r && !pal_menu_row_hidden(r) && !r->inert && r->kind != PAL_MENU_SECTION && !pal_menu_gauge(r);
}

static const pal_menu_row_t *row_at(const pal_menu_page_t *p, size_t i) {
    return p && i < p->n ? &p->rows[i] : NULL;
}

bool pal_menu_has_selectable(const pal_menu_page_t *p) {
    for (size_t i = 0; p && i < p->n; i++)
        if (pal_menu_selectable(&p->rows[i])) return true;
    return false;
}

/* Keeps sel on a selectable row (the first one when the page changed under it). */
static void fix_sel(pal_menu_page_t *p) {
    int was = p->sel;
    if (p->sel < 0 || (size_t)p->sel >= p->n || !pal_menu_selectable(row_at(p, (size_t)p->sel))) {
        p->sel = 0;
        for (size_t i = 0; i < p->n; i++)
            if (pal_menu_selectable(row_at(p, i))) {
                p->sel = (int)i;
                break;
            }
    }
    if (p->sel != was) p->follow = true; /* a rebuild that leaves the selection where it was does not move the view */
    if (p->scroll > p->sel) p->scroll = p->sel;
    if (p->scroll < 0) p->scroll = 0;
}

static void build(pal_menu_page_t *p) {
    if (!p->build) return;
    if (p->store) {
        p->rows = p->store;
        p->n = 0;
    }
    p->build(p);
    fix_sel(p);
}

void pal_menu_clear(pal_menu_page_t *p) {
    if (!p) return;
    p->n = 0;
    if (p->store) p->rows = p->store;
}

bool pal_menu_add(pal_menu_page_t *p, const pal_menu_row_t *r) {
    if (!p || !p->store || !r || p->n >= p->cap) return false;
    p->store[p->n++] = *r;
    p->rows = p->store;
    return true;
}

void pal_menu_init(pal_menu_t *m, pal_menu_page_t *root) {
    memset(m, 0, sizeof(*m));
    if (root) pal_menu_push(m, root);
}

void pal_menu_reset(pal_menu_t *m, pal_menu_page_t *root) {
    m->depth = 0;
    m->pend = PAL_MENU_PEND_NONE;
    m->pend_page = NULL;
    m->done_ask = NULL;
    m->done_confirm = NULL;
    if (root) pal_menu_push(m, root);
}

bool pal_menu_push(pal_menu_t *m, pal_menu_page_t *p) {
    if (!p || m->depth >= PAL_MENU_DEPTH) return false;
    m->stack[m->depth++] = p;
    build(p);
    fix_sel(p);
    p->follow = true; /* opened: the selection is in view */
    return true;
}

/* Coming back to a dynamic page rebuilds it: what it shows may have changed while it was covered (a Wi-Fi join). */
bool pal_menu_pop(pal_menu_t *m) {
    if (m->depth <= 1) return false;
    m->depth--;
    build(m->stack[m->depth - 1]);
    m->stack[m->depth - 1]->follow = true; /* back on it: the selection is in view */
    return true;
}

pal_menu_page_t *pal_menu_top(const pal_menu_t *m) { return m->depth > 0 ? m->stack[m->depth - 1] : NULL; }
int pal_menu_depth(const pal_menu_t *m) { return m->depth; }

void pal_menu_refresh(pal_menu_t *m) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p) return;
    build(p);
    fix_sel(p);
}

/* ---- selection ---- */

pal_menu_row_t *pal_menu_selected(pal_menu_t *m) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p || p->sel < 0 || (size_t)p->sel >= p->n) return NULL;
    return (pal_menu_row_t *)&p->rows[p->sel];
}

bool pal_menu_select(pal_menu_t *m, size_t index) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p || !pal_menu_selectable(row_at(p, index))) return false;
    p->sel = (int)index;
    p->follow = true;
    return true;
}

static void move(pal_menu_page_t *p, int dir) {
    size_t n = p->n;
    if (n == 0) return;
    int i = p->sel;
    for (size_t k = 0; k < n; k++) {
        i = ((i + dir) % (int)n + (int)n) % (int)n; /* in int: -1 must wrap to the last row, not through an unsigned -1 */
        if (pal_menu_selectable(row_at(p, (size_t)i))) {
            p->sel = i;
            p->follow = true;
            return;
        }
    }
}

/* ---- values ---- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int choice_count(const pal_menu_row_t *r) {
    int n = 0;
    while (r->choices && r->choices[n]) n++;
    return n;
}

/* Flips a toggle, cycles a choice or steps a slider by dir. Returns true when the value changed. */
static bool change(pal_menu_row_t *r, int dir, bool big) {
    if (!r->set) return false;
    switch (r->kind) {
    case PAL_MENU_TOGGLE: {
        int cur = r->get ? r->get(r->ctx) : 0;
        r->set(r->ctx, cur ? 0 : 1);
        return true;
    }
    case PAL_MENU_CHOICE: {
        int n = choice_count(r);
        if (n == 0) return false;
        int cur = r->get ? r->get(r->ctx) : 0;
        r->set(r->ctx, (((cur + dir) % n) + n) % n);
        return true;
    }
    case PAL_MENU_SLIDER: {
        int cur = r->get ? r->get(r->ctx) : r->min;
        int step = r->step > 0 ? r->step : 1;
        if (big) step *= 4;
        int v = clampi(cur + dir * step, r->min, r->max);
        if (v == cur) return false;
        r->set(r->ctx, v);
        return true;
    }
    default:
        return false;
    }
}

bool pal_menu_step(pal_menu_t *m, int dir, bool big) {
    pal_menu_row_t *r = pal_menu_selected(m);
    if (!r || r->disabled) return false;
    if (r->kind == PAL_MENU_TOGGLE && !r->set) return false;
    return change(r, dir < 0 ? -1 : 1, big);
}

/* ---- the asking calls ---- */

void pal_menu_ask_text(pal_menu_t *m, const char *title, const char *initial, int max_len, bool mask,
                       void (*done)(void *ctx, bool ok, const char *text), void *ctx) {
    m->pend = PAL_MENU_PEND_ASK;
    m->pend_page = NULL;
    m->done_ask = done;
    m->done_ctx = ctx;
    ask_text(m, title, initial, max_len, mask);
}

void pal_menu_confirm(pal_menu_t *m, const char *title, const char *text, const char *ok,
                      void (*done)(void *ctx, bool ok), void *ctx) {
    m->pend = PAL_MENU_PEND_CONFIRM;
    m->pend_page = NULL;
    m->done_confirm = done;
    m->done_ctx = ctx;
    ask_confirm(m, title, text, ok);
}

/* The pending row is still the one on top of the stack (a page can be popped while a question is open: then it is void). */
static pal_menu_row_t *pending_row(pal_menu_t *m) {
    if (!m->pend_page || pal_menu_top(m) != m->pend_page || m->pend_row >= m->pend_page->n) return NULL;
    return (pal_menu_row_t *)&m->pend_page->rows[m->pend_row];
}

void pal_menu_answer_pick(pal_menu_t *m, int index) {
    if (m->pend != PAL_MENU_PEND_PICK) return;
    pal_menu_row_t *r = pending_row(m);
    m->pend = PAL_MENU_PEND_NONE;
    if (index >= 0 && r && r->set) r->set(r->ctx, index);
}

void pal_menu_answer_confirm(pal_menu_t *m, bool ok) {
    if (m->pend != PAL_MENU_PEND_CONFIRM) return;
    m->pend = PAL_MENU_PEND_NONE;
    if (!m->pend_page) {
        void (*done)(void *, bool) = m->done_confirm;
        m->done_confirm = NULL;
        if (done) done(m->done_ctx, ok);
        return;
    }
    pal_menu_row_t *r = pending_row(m);
    m->pend_page = NULL; /* cleared first: the action may open a new question */
    if (ok && r && r->run) r->run(r->ctx);
}

void pal_menu_answer_ask(pal_menu_t *m, bool ok, const char *text) {
    if (m->pend != PAL_MENU_PEND_ASK) return;
    m->pend = PAL_MENU_PEND_NONE;
    if (!m->pend_page) {
        void (*done)(void *, bool, const char *) = m->done_ask;
        m->done_ask = NULL;
        if (done) done(m->done_ctx, ok, ok ? text : NULL);
        return;
    }
    pal_menu_row_t *r = pending_row(m);
    m->pend_page = NULL;
    if (!ok || !r || !r->apply_text) return;
    if (!r->apply_text(r->ctx, text ? text : "")) pal_menu_toast("Not saved");
}

/* A row asks a question: the row is remembered so its answer can find it again. */
static void remember(pal_menu_t *m, pal_menu_pend_t kind) {
    m->pend = kind;
    m->pend_page = pal_menu_top(m);
    m->pend_row = (size_t)m->pend_page->sel;
    m->done_ask = NULL;
    m->done_confirm = NULL;
}

bool pal_menu_activate(pal_menu_t *m) {
    pal_menu_row_t *r = pal_menu_selected(m);
    if (!r) return false;
    if (r->disabled) { /* a row that cannot be used says why: its hint, or a plain "not now" */
        pal_menu_toast(r->hint && r->hint[0] ? r->hint : "Not available now");
        return true;
    }
    switch (r->kind) {
    case PAL_MENU_SUBMENU: {
        if (!r->open) return false;
        pal_menu_page_t *p = r->open(r->ctx);
        return p && pal_menu_push(m, p);
    }
    case PAL_MENU_TOGGLE:
        return change(r, 1, false);
    case PAL_MENU_CHOICE: {
        int n = choice_count(r);
        if (n == 0 || !r->set) return false;
        if (n > PAL_PICKER_ITEMS) n = PAL_PICKER_ITEMS;
        remember(m, PAL_MENU_PEND_PICK);
        ask_picker(m, r->label, r->choices, n);
        return true;
    }
    case PAL_MENU_ACTION:
        if (r->confirm) {
            remember(m, PAL_MENU_PEND_CONFIRM);
            ask_confirm(m, r->label, r->confirm, r->ok_label ? r->ok_label : "OK");
            return true;
        }
        if (!r->run) return false;
        r->run(r->ctx);
        return true;
    case PAL_MENU_TEXT: {
        if (!r->apply_text) return false;
        char init[PAL_MENU_TEXT_MAX] = "";
        if (r->text) r->text(r->ctx, init, sizeof(init));
        remember(m, PAL_MENU_PEND_ASK);
        int max = r->max_len > 0 ? r->max_len : PAL_MENU_TEXT_MAX - 1;
        ask_text(m, r->title ? r->title : r->label, init, max, r->mask);
        return true;
    }
    default:
        return false;
    }
}

bool pal_menu_key(pal_menu_t *m, const pal_key_t *k) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p) return false;
    bool big = (k->mods & PAL_MOD_SHIFT) != 0;
    if (!pal_menu_has_selectable(p)) { /* information only: the keys scroll the page */
        switch (pal_nav_of(k)) {
        case PAL_NAV_UP: (void)pal_menu_scroll(m, -1, false); return true;
        case PAL_NAV_DOWN: (void)pal_menu_scroll(m, 1, false); return true;
        case PAL_NAV_LEFT: (void)pal_menu_scroll(m, -1, true); return true;
        case PAL_NAV_RIGHT: (void)pal_menu_scroll(m, 1, true); return true;
        default: break;
        }
    }
    switch (pal_nav_of(k)) {
    case PAL_NAV_UP:
        if (!pal_menu_reveal(m, -1)) move(p, -1);
        return true;
    case PAL_NAV_DOWN:
        if (!pal_menu_reveal(m, 1)) move(p, 1);
        return true;
    case PAL_NAV_LEFT:
        return pal_menu_step(m, -1, big);
    case PAL_NAV_RIGHT:
        return pal_menu_step(m, 1, big);
    case PAL_NAV_OK:
        return pal_menu_activate(m);
    case PAL_NAV_BACK:
        return pal_menu_pop(m);
    default:
        return false;
    }
}

/* ---- bindings for desktops (pal/menu.h) ---- */

static struct {
    char id[24];
    pal_menu_t *m;
} s_bound[4] NEMA_EXT_BSS;

void pal_menu_bind(const char *view_id, pal_menu_t *m) {
    if (!view_id) return;
    int free_slot = -1;
    for (int i = 0; i < 4; i++) {
        if (s_bound[i].m && !strcmp(s_bound[i].id, view_id)) {
            s_bound[i].m = m;
            return;
        }
        if (!s_bound[i].m && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || !m) return;
    nema_strlcpy(s_bound[free_slot].id, view_id, sizeof(s_bound[free_slot].id));
    s_bound[free_slot].m = m;
}

pal_menu_t *pal_menu_bound(const char *view_id) {
    if (!view_id) return NULL;
    for (int i = 0; i < 4; i++)
        if (s_bound[i].m && !strcmp(s_bound[i].id, view_id)) return s_bound[i].m;
    return NULL;
}

void pal_menu_value_text(const pal_menu_row_t *r, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    switch (r->kind) {
    case PAL_MENU_TOGGLE: snprintf(out, cap, "%s", r->get && r->get(r->ctx) ? "On" : "Off"); break;
    case PAL_MENU_CHOICE: {
        int v = r->get ? r->get(r->ctx) : 0, n = 0;
        while (r->choices && r->choices[n]) n++;
        if (v >= 0 && v < n) snprintf(out, cap, "%s", r->choices[v]);
        break;
    }
    case PAL_MENU_SLIDER:
        if (r->get) snprintf(out, cap, "%d", r->get(r->ctx));
        break;
    case PAL_MENU_TEXT:
        if (r->text) {
            r->text(r->ctx, out, cap);
            if (r->mask)
                for (char *p = out; *p; p++) *p = '*';
        }
        break;
    case PAL_MENU_INFO:
    case PAL_MENU_SUBMENU:
    case PAL_MENU_ACTION:
        if (r->text && (r->kind != PAL_MENU_ACTION || r->show_value)) r->text(r->ctx, out, cap);
        break;
    default: break;
    }
}

bool pal_menu_row_hidden(const pal_menu_row_t *r) {
    if (r->hidden || (r->kind == PAL_MENU_SECTION && r->group)) return true;
    if (r->owner && strcmp(r->owner, pal_desktops_active()) != 0) return true;
    return r->key && !pal_setting_visible(r->key);
}
