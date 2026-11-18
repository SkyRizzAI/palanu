/* PalanuOS desktop: the compositor. Layers bottom to top: the front view, the status bar, the overlay stack
 * (overlay.c), the toast layer. Keys: the lock view gets them all while locked; otherwise the global keys (cycle tap opens
 * the switcher, hold goes home), then the top overlay, the home bindings, the front view, and Esc as back.
 * Everything that changes the layers runs on this task: other tasks post the change (desk_post). */
#include <stdio.h>
#include <string.h>

#include "appui.h"
#include "desktop_int.h"
#include "nema/log.h"
#include "nema/nema.h"
#include "pal/console.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/dpm.h"
#include "pal/kui.h"
#include "pal/notify.h"
#include "pal/overlay.h"
#include "pal/settings.h"
#include "pal/input_menu.h"
#include "pal/vk.h"
#include "pal/time.h"
#include "pal/toast.h"

#define MAX_VIEWS 20
#define MAX_WATCH 5
#define MAX_POSTS 8
#define ID_MAX 24
#define BINDS 8

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} post_t;

typedef struct {
    bool used;
    uint8_t sym;
    char id[ID_MAX];
} bind_t;

static struct {
    pal_desktop_cfg_t cfg;
    pal_view_t *views[MAX_VIEWS];
    const char *vowner[MAX_VIEWS]; /* the session that registered the view, NULL for the system's own (ADR-0010) */
    const char *reg_owner;         /* the session whose start is running: what it registers is its own */
    int nviews;
    int mru[MAX_VIEWS]; /* indices, most recent first; mru[0] is the front view */
    int nmru;
    nema_obj_t *watch[MAX_WATCH];
    int nwatch;
    nema_obj_t wake;
    volatile bool inval, inval_all, status_dirty;
    nema_port_mutex_t *lock;
    nema_owner_t owner;
    uint32_t last_status_poll;
    pal_status_t last_status; /* what the status bar last showed */
    pal_status_t polled;      /* the last once-a-second reading (a desktop's own bar reads this, never the ADC) */
    bool polled_ok;
    volatile bool started;
    uint32_t frames; /* renders that drew the front view */
    const char *start_view;
    char home[ID_MAX];
    char lock_id[ID_MAX];
    char pending[ID_MAX]; /* the view a cross-task open asked for (latest request wins) */
    post_t posts[MAX_POSTS];
    int phead;
    volatile int npost;
    pal_status_item_t items[12];
    int nitems;
    bind_t binds[BINDS];
    pal_view_t *cur; /* the view the desktop is running now: pal_legend_set changes its legends */
    const pal_desktop_chrome_t *chrome; /* the desktop environment's chrome; NULL = classic */
    bool owned; /* the chrome drew the whole screen at the last frame */
} D;

/* The desktop task is recognised by its per-task context: nema keeps one pointer of thread-local storage per task (its wait
 * context), and the task records that pointer once it runs. The slot itself is never written here. */
static void *desk_tls;

static int w_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static int w_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static uint32_t w_poll(nema_obj_t *o, uint32_t e) { (void)o, (void)e; return (D.inval || D.inval_all) ? NEMA_POLL_IN : 0; }
static nema_err_t w_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t WAKE_OPS = {"desktop", w_read, w_write, w_poll, w_ioctl, NULL};

static uint32_t now_ms(void) { return (uint32_t)(nema_port_time_us() / 1000u); }

/* ---- cross-task work ---- */

bool desk_on_task(void) {
    void *t = nema_port_tls_get();
    return t != NULL && t == desk_tls;
}
bool desk_started(void) { return D.started; }
void desk_wake(void) { nema_notify(&D.wake); }
void desk_lock(void) { nema_port_mutex_lock(D.lock); }
void desk_unlock(void) { nema_port_mutex_unlock(D.lock); }

nema_err_t desk_post(void (*fn)(void *arg), void *arg) {
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(D.lock);
    if (D.npost == MAX_POSTS) r = NEMA_ERR_FULL;
    else {
        D.posts[(D.phead + D.npost) % MAX_POSTS] = (post_t){fn, arg};
        D.npost++;
    }
    nema_port_mutex_unlock(D.lock);
    if (r == NEMA_OK) nema_notify(&D.wake);
    return r;
}

static void run_posts(void) {
    for (;;) {
        post_t p;
        nema_port_mutex_lock(D.lock);
        if (D.npost == 0) {
            nema_port_mutex_unlock(D.lock);
            return;
        }
        p = D.posts[D.phead];
        D.phead = (D.phead + 1) % MAX_POSTS;
        D.npost--;
        nema_port_mutex_unlock(D.lock);
        p.fn(p.arg);
    }
}

/* Runs fn now when this is the desktop task (or before it starts); otherwise queues it. */
static nema_err_t run_or_post(void (*fn)(void *arg), void *arg) {
    if (!D.started || desk_on_task()) {
        fn(arg);
        return NEMA_OK;
    }
    return desk_post(fn, arg);
}

/* ---- views ---- */

static pal_view_t *front(void) { return D.nmru ? D.views[D.mru[0]] : NULL; }
pal_view_t *desk_front(void) { return front(); }
int desk_mru_count(void) { return D.nmru; }
pal_view_t *desk_mru_at(int i) { return i >= 0 && i < D.nmru ? D.views[D.mru[i]] : NULL; }
const char *desk_home_id(void) { return D.home; }

static int bar_h(void) { return kui_top_px(KUI_STATUS_H); } /* The reference status bar: 11 logical rows, 22 px at 2x (kui_status.c) */
/* Settings: ui.statusbar. Off hides the bar everywhere (the layers take the whole screen). */
static bool bar_on(void) { return !(D.chrome && D.chrome->hide_bar) && psys_settings_int("ui.statusbar", 1) != 0; }
/* Rows the desktop environment keeps for its own bar (pal_desktop_chrome_t.top_px). */
static int chrome_top(void) { return D.chrome ? D.chrome->top_px : 0; }

/* Legends are shown on the home screen only for now (the owner's call: Settings and the other screens are cleaner without the bar).
 * Every view keeps declaring its legends and the API is unchanged; pal_legend_home_only(false) shows them again. */
static bool s_legend_home_only = true;
void pal_legend_home_only(bool on) { s_legend_home_only = on; }

/* The view whose legends the legend bar shows (pal/legend.h). Walks down from the top overlay: an overlay with no legends of
 * its own (a dialog) lets the layer under it show its legends; an opaque one with none, or a NOLEGEND one, hides the bar. */
static pal_view_t *legend_owner_any(void) {
    if (psys_settings_int("ui.legend", 1) == 0 || (D.chrome && D.chrome->hide_legend)) return NULL;
    for (int i = ovl_count() - 1; i >= 0; i--) {
        pal_view_t *v = ovl_view(i);
        uint32_t fl = ovl_flags(i);
        if (fl & PAL_VIEW_NOLEGEND) return NULL;
        if (v->nlegends > 0) return v;
        if (fl & PAL_VIEW_OPAQUE) return NULL;
    }
    pal_view_t *f = front();
    if (!f || (f->flags & PAL_VIEW_NOLEGEND) || f->nlegends <= 0) return NULL;
    return f;
}

static pal_view_t *legend_owner(void) {
    pal_view_t *v = legend_owner_any();
    if (v && s_legend_home_only && strcmp(v->id, D.home) != 0) return NULL;
    return v;
}

/* The legend bar's height while it shows; every layer's area stops above it. */
static int legend_h(void) { return legend_owner() ? pal_legend_h_px() : 0; }

/* The area of a layer with these flags: below the status bar unless it is FULLSCREEN or NOBAR, and above the legend bar. */
static pal_rect_t area_of(uint32_t flags) {
    pal_canvas_t *c = pal_display_canvas();
    int top = ((flags & (PAL_VIEW_FULLSCREEN | PAL_VIEW_NOBAR)) || !bar_on() || !pal_theme_active()->comp.status_bar) ? 0 : bar_h();
    if (chrome_top() > top) top = chrome_top(); /* the desktop's own bar stays on screen over every layer */
    pal_rect_t r = {0, top, c->w, c->h - top - legend_h()};
    return r;
}

pal_rect_t pal_desktop_area(const pal_view_t *v) { return area_of(v->flags); }

nema_err_t pal_desktop_register(pal_view_t *v) {
    if (!v || !v->id) return NEMA_ERR_INVAL;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(D.lock);
    for (int i = 0; i < D.nviews; i++)
        if (strcmp(D.views[i]->id, v->id) == 0) r = NEMA_ERR_EXIST;
    if (r == NEMA_OK) {
        if (D.nviews == MAX_VIEWS) r = NEMA_ERR_FULL;
        else {
            D.vowner[D.nviews] = D.reg_owner;
            D.views[D.nviews++] = v;
        }
    }
    nema_port_mutex_unlock(D.lock);
    return r;
}

pal_view_t *pal_desktop_view(const char *id) {
    for (int i = 0; i < D.nviews; i++)
        if (strcmp(D.views[i]->id, id) == 0) return D.views[i];
    return NULL;
}
size_t pal_desktop_view_count(void) { return (size_t)D.nviews; }
pal_view_t *pal_desktop_view_at(size_t i) { return i < (size_t)D.nviews ? D.views[i] : NULL; }
const char *pal_desktop_front_id(void) { return front() ? front()->id : ""; }

/* ---- sessions (pal/desktops.h, ADR-0010): what a session registers is its own, and its stop takes all of it away ---- */

void desk_session_scope(const char *id) { D.reg_owner = id; }

size_t desk_session_views(const char *id) {
    size_t n = 0;
    for (int i = 0; i < D.nviews; i++)
        if (D.vowner[i] && id && !strcmp(D.vowner[i], id)) n++;
    return n;
}

static bool owned_by(int i, const char *id) { return D.vowner[i] && id && !strcmp(D.vowner[i], id); }

/* Desktop task only (or before it starts). Every view of the session leaves the overlays and the views, and everything a session
 * sets on the compositor goes back to nothing: chrome, home, lock view, home keys and the rows kept for a bar. */
void desk_session_reset(const char *id) {
    for (int i = ovl_count() - 1; i >= 0; i--) {
        pal_view_t *ov = ovl_view(i);
        for (int j = 0; j < D.nviews; j++)
            if (D.views[j] == ov && owned_by(j, id)) {
                ovl_remove_now(ov);
                break;
            }
    }
    pal_view_t *f = front();
    for (int j = 0; j < D.nviews; j++)
        if (D.views[j] == f && owned_by(j, id) && f->leave) {
            D.cur = f;
            f->leave(f);
        }
    int map[MAX_VIEWS], n = 0;
    nema_port_mutex_lock(D.lock);
    for (int i = 0; i < D.nviews; i++) {
        if (owned_by(i, id)) {
            map[i] = -1;
            continue;
        }
        map[i] = n;
        D.views[n] = D.views[i];
        D.vowner[n] = D.vowner[i];
        n++;
    }
    D.nviews = n;
    int m = 0;
    for (int i = 0; i < D.nmru; i++)
        if (map[D.mru[i]] >= 0) D.mru[m++] = map[D.mru[i]];
    D.nmru = m;
    D.chrome = NULL;
    D.owned = false;
    D.home[0] = 0;
    D.lock_id[0] = 0;
    for (int i = 0; i < BINDS; i++) D.binds[i].used = false;
    nema_port_mutex_unlock(D.lock);
    if (pal_dpm_locked()) pal_dpm_set_locked(false, now_ms()); /* its lock screen is gone with it */
    pal_display_reserve_top(0);
    D.inval_all = true;
    if (pal_display_canvas()) pal_mark_all(pal_display_canvas()); /* no display (a host test): nothing to redraw */
    nema_notify(&D.wake);
}

nema_err_t pal_desktop_watch(nema_obj_t *o) {
    nema_err_t r = NEMA_ERR_FULL;
    nema_port_mutex_lock(D.lock);
    if (D.nwatch < MAX_WATCH) {
        D.watch[D.nwatch++] = o;
        r = NEMA_OK;
    }
    nema_port_mutex_unlock(D.lock);
    nema_notify(&D.wake);
    return r;
}

void pal_desktop_unwatch(nema_obj_t *o) {
    nema_port_mutex_lock(D.lock);
    for (int i = 0; i < D.nwatch; i++) {
        if (D.watch[i] == o) {
            D.watch[i] = D.watch[--D.nwatch];
            break;
        }
    }
    nema_port_mutex_unlock(D.lock);
}

void pal_desktop_invalidate(void) {
    D.inval = true;
    nema_notify(&D.wake);
}
void pal_desktop_invalidate_all(void) {
    D.inval_all = true;
    nema_notify(&D.wake);
}
void pal_status_changed(void) {
    D.status_dirty = true;
    nema_notify(&D.wake);
}

/* ---- switching ---- */
static void activate(int idx) {
    pal_view_t *old = front();
    if (old && D.views[idx] == old) return;
    if (old && old->leave) {
        D.cur = old;
        old->leave(old);
    }
    int pos = -1;
    for (int i = 0; i < D.nmru; i++)
        if (D.mru[i] == idx) pos = i;
    if (pos < 0) pos = D.nmru < MAX_VIEWS ? D.nmru++ : D.nmru - 1;
    for (int i = pos; i > 0; i--) D.mru[i] = D.mru[i - 1];
    D.mru[0] = idx;
    pal_view_t *v = D.views[idx];
    D.cur = v;
    if (v->enter) v->enter(v);
    D.inval_all = true;
}

/* Desktop task only. A locked device opens nothing else. */
void desk_open_now(const char *id) {
    if (pal_dpm_locked()) return;
    for (int i = 0; i < D.nviews; i++) {
        if (strcmp(D.views[i]->id, id) == 0) {
            ovl_clear_now();
            activate(i);
            nema_notify(&D.wake);
            return;
        }
    }
}

static void post_open(void *arg) {
    (void)arg;
    char id[ID_MAX];
    nema_port_mutex_lock(D.lock);
    memcpy(id, D.pending, sizeof(id));
    nema_port_mutex_unlock(D.lock);
    desk_open_now(id);
}

void pal_desktop_open(const char *id) {
    if (!id) return;
    nema_port_mutex_lock(D.lock);
    nema_strlcpy(D.pending, id, sizeof(D.pending));
    nema_port_mutex_unlock(D.lock);
    run_or_post(post_open, NULL);
}

void pal_desktop_set_home(const char *id) {
    if (!id || !id[0]) return;
    nema_port_mutex_lock(D.lock);
    nema_strlcpy(D.home, id, sizeof(D.home));
    nema_port_mutex_unlock(D.lock);
}
const char *pal_desktop_home_id(void) { return D.home; }

void pal_desktop_home(void) {
    if (D.home[0]) pal_desktop_open(D.home);
}

/* The app in front is done: the session decides what comes next (its home). Apps never name a session's view. */
void pal_app_exit(void) { pal_desktop_home(); }

/* ---- lock ---- */

static pal_view_t *lock_view(void) { return D.lock_id[0] ? pal_desktop_view(D.lock_id) : NULL; }
bool desk_locked_view(const pal_view_t *v) { return pal_dpm_locked() && v && v == lock_view(); }

static nema_err_t lock_now(void) {
    pal_view_t *lv = lock_view();
    if (!lv) return NEMA_ERR_NOENT;
    if (pal_dpm_locked()) return NEMA_OK;
    nema_err_t r = ovl_push_now(lv, PAL_VIEW_OPAQUE);
    if (r != NEMA_OK) return r;
    pal_dpm_set_locked(true, now_ms());
    D.status_dirty = true;
    pal_desktop_invalidate_all();
    return NEMA_OK;
}

static void unlock_now(void) {
    if (!pal_dpm_locked()) return;
    pal_dpm_set_locked(false, now_ms());
    pal_view_t *lv = lock_view();
    if (lv) ovl_remove_now(lv);
    D.status_dirty = true;
    pal_desktop_invalidate_all();
}

static void post_lock(void *arg) { (void)arg; lock_now(); }
static void post_unlock(void *arg) { (void)arg; unlock_now(); }

void pal_desktop_set_lock_view(const char *view_id) {
    if (!view_id) return;
    nema_port_mutex_lock(D.lock);
    nema_strlcpy(D.lock_id, view_id, sizeof(D.lock_id));
    nema_port_mutex_unlock(D.lock);
}

nema_err_t pal_desktop_lock(void) {
    if (!D.lock_id[0]) return NEMA_ERR_NOENT; /* no session, or one without a lock screen */
    if (!D.started || desk_on_task()) return lock_now();
    return desk_post(post_lock, NULL);
}

void pal_desktop_unlock(void) { run_or_post(post_unlock, NULL); }
bool pal_desktop_locked(void) { return pal_dpm_locked(); }

void pal_desktop_activity(void) { pal_dpm_activity(now_ms()); }

/* ---- display power ---- */

static void dpm_display(bool on) { pal_display_backlight(on); }
static bool dpm_lock(void) { return lock_now() == NEMA_OK; }

/* ---- status bar ---- */

static pal_status_t status_now(void) {
    pal_status_t st = {0, 0, -1, 0, false, false, false};
    if (D.cfg.status) st = D.cfg.status();
    pal_tm_t tm;
    pal_time_local(&tm);
    st.hour = tm.hour;
    st.minute = tm.minute;
    return st;
}

static bool status_same(const pal_status_t *a, const pal_status_t *b) {
    return a->hour == b->hour && a->minute == b->minute && a->battery == b->battery && a->wifi == b->wifi &&
           a->link == b->link && a->ble == b->ble && a->sd == b->sd;
}

/* The status bar is drawn when the top layer allows it. */
static bool bar_visible(void) {
    int n = ovl_count();
    uint32_t fl = n ? ovl_flags(n - 1) : (front() ? front()->flags : 0);
    if (!pal_theme_active()->comp.status_bar || !bar_on()) return false;
    if (fl & PAL_VIEW_NOBAR) return false;
    if (!n && (fl & PAL_VIEW_FULLSCREEN)) return false;
    return true;
}

static void draw_bar(pal_canvas_t *c, const pal_status_t *st) {
    const pal_theme_t *t = pal_theme_active();
    pal_clip_reset(c);
    t->comp.status_bar(c, t, st);
    int h = bar_h(), x = kui_top_px(KUI_STATUS_ITEMS_X); /* right of the clock at any UI scale */
    for (int i = 0; i < D.nitems; i++) {
        int w = D.items[i].draw(c, t, x, h, st);
        if (w > 0) x += w + 4;
    }
    D.last_status = *st;
}

static void draw_layer(pal_view_t *v, uint32_t flags, bool full) {
    if (!v || !v->draw) return;
    pal_canvas_t *c = pal_display_canvas();
    pal_rect_t a = area_of(flags);
    pal_clip_set(c, a.x, a.y, a.w, a.h);
    D.cur = v;
    v->draw(v, c, a, full);
    pal_clip_reset(c);
}

/* The reference toast: a full-width band right under the status bar (kui_toast_band), not a box in the middle. */
static void draw_toast(pal_canvas_t *c) {
    if (!pal_toast_showing() || (D.chrome && D.chrome->services)) return; /* a session that draws the services shows its toasts */
    const pal_theme_t *t = pal_theme_active();
    char text[PAL_TOAST_CHARS + 1];
    nema_strlcpy(text, pal_toast_text(), sizeof(text));
    pal_rect_t box = {0, bar_h(), c->w, kui_top_px(KUI_STATUS_H + KUI_TOAST_LH) - bar_h()};
    pal_clip_reset(c);
    t->comp.toast(c, t, box, text);
}

static void full_redraw(pal_canvas_t *c) {
    pal_clip_reset(c);
    pal_fill(c, 0, 0, c->w, c->h, PAL_C_PAPER);
    int base = ovl_base();
    if (base < 0) draw_layer(front(), front()->flags, true);
    if (bar_visible()) {
        pal_status_t st = status_now();
        draw_bar(c, &st);
    }
    for (int i = base < 0 ? 0 : base; i < ovl_count(); i++) draw_layer(ovl_view(i), ovl_flags(i), true);
    pal_view_t *lo = legend_owner();
    if (lo) pal_legend_draw(c, lo->legends, lo->nlegends);
    draw_toast(c);
}

static void render(void) {
    if (!pal_dpm_display_on()) return; /* asleep: the flags stay set; waking the screen redraws everything */
    pal_canvas_t *c = pal_display_canvas();
    if (pal_console_front()) { /* the system console has the screen: nothing of the session is drawn */
        bool full = D.inval_all;
        D.inval = D.inval_all = D.status_dirty = false;
        D.owned = true; /* when it gives the screen back, everything is drawn again */
        pal_console_render(full);
        return;
    }
    if (!front()) return;
    if (D.chrome && D.chrome->owns_screen && D.chrome->owns_screen()) { /* the desktop draws the screen itself now */
        D.owned = true;
        D.inval = D.inval_all = D.status_dirty = false; /* nothing of ours to draw; left set, they would wake this loop forever */
        return;
    }
    if (D.owned) { /* it gave the screen back: everything under it is stale */
        D.owned = false;
        D.inval_all = true;
        pal_mark_all(c);
    }
    if (D.inval_all) {
        D.inval = D.inval_all = false;
        D.status_dirty = false;
        full_redraw(c);
        if (ovl_base() < 0 && ovl_count() == 0) D.frames++;
    } else {
        if (D.inval) {
            D.inval = false;
            int n = ovl_count();
            if (n) draw_layer(ovl_view(n - 1), ovl_flags(n - 1), false);
            else {
                draw_layer(front(), front()->flags, false);
                D.frames++;
            }
        }
        if (D.status_dirty) {
            D.status_dirty = false;
            if (bar_visible()) {
                pal_status_t st = status_now();
                draw_bar(c, &st);
            }
        }
        draw_toast(c);
    }
    pal_display_flush();
    appui_after_render(); /* an app's canvas over its area (api/palanu/ui.h) */
}

/* ---- keys ---- */

static bool bind_key(const pal_key_t *k) {
    pal_view_t *f = front();
    if (!f || strcmp(f->id, D.home) != 0) return false;
    for (int i = 0; i < BINDS; i++) {
        if (!D.binds[i].used || D.binds[i].sym != k->sym) continue;
        pal_view_t *v = pal_desktop_view(D.binds[i].id);
        if (v && ovl_push_now(v, PAL_VIEW_OPAQUE) == NEMA_OK) return true;
        return false;
    }
    return false;
}

static void handle_key(const pal_key_t *k) {
    if (pal_dpm_key(now_ms())) { /* the key only woke the screen: it is consumed */
        D.inval_all = true;
        return;
    }
    if (pal_console_key(k)) return; /* the system console's switch key, and every key while it is in front (ADR-0010) */
    if (pal_dpm_locked()) { /* while locked, the lock view gets every key and nothing else does */
        pal_view_t *lv = lock_view();
        if (lv && lv->key) {
            D.cur = lv;
            lv->key(lv, k);
        }
        D.inval = true;
        return;
    }
    if (D.chrome && D.chrome->key && D.chrome->key(k)) { /* the desktop environment's own keys */
        D.inval = true;
        return;
    }
    if (k->sym == PAL_KEY_GOHOME) { /* the session's home; with no session, nothing */
        if (D.home[0]) desk_open_now(D.home);
        return;
    }
    pal_view_t *ov = ovl_count() ? ovl_view(ovl_count() - 1) : NULL;
    if (ov) {
        D.cur = ov;
        bool handled = ov->key && ov->key(ov, k);
        if (!handled && k->sym == PAL_KEY_ESC) ovl_remove_now(ov);
        D.inval = true;
        return;
    }
    if (bind_key(k)) {
        D.inval = true;
        return;
    }
    pal_view_t *v = front();
    D.cur = v;
    bool handled = v && v->key && v->key(v, k);
    if (!handled && k->sym == PAL_KEY_ESC && v && D.home[0] && strcmp(v->id, D.home) != 0) desk_open_now(D.home); /* back to the session */
    pal_desktop_invalidate();
}

nema_err_t pal_desktop_bind_home_key(uint8_t sym, const char *view_id) {
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(D.lock);
    int slot = -1;
    for (int i = 0; i < BINDS; i++)
        if (D.binds[i].used && D.binds[i].sym == sym) slot = i;
    if (!view_id) {
        if (slot >= 0) D.binds[slot].used = false;
    } else {
        if (slot < 0)
            for (int i = 0; i < BINDS && slot < 0; i++)
                if (!D.binds[i].used) slot = i;
        if (slot < 0) r = NEMA_ERR_FULL;
        else {
            D.binds[slot].used = true;
            D.binds[slot].sym = sym;
            nema_strlcpy(D.binds[slot].id, view_id, sizeof(D.binds[slot].id));
        }
    }
    nema_port_mutex_unlock(D.lock);
    return r;
}

/* ---- legend bar (pal/legend.h) ---- */

void pal_legend_set(const pal_legend_item_t *items, int n) {
    pal_view_t *v = D.cur;
    if (!v || !desk_on_task()) return;
    if (n <= 0) {
        items = NULL;
        n = 0;
    }
    if (v->legends == items && v->nlegends == n) return;
    v->legends = items;
    v->nlegends = n;
    pal_desktop_invalidate_all(); /* the bar and the content area may change */
}

void pal_legend_clear(void) { pal_legend_set(NULL, 0); }

/* ---- status items registry ---- */

nema_err_t pal_status_item_register(const pal_status_item_t *item) {
    if (!item || !item->draw) return NEMA_ERR_INVAL;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(D.lock);
    if (D.nitems == (int)(sizeof(D.items) / sizeof(D.items[0]))) r = NEMA_ERR_FULL;
    else D.items[D.nitems++] = *item;
    nema_port_mutex_unlock(D.lock);
    if (r == NEMA_OK) pal_status_changed();
    return r;
}

/* ---- the task ---- */

static bool pred_work(void *ctx) {
    (void)ctx;
    if (D.npost > 0) return true;
    if (pal_dpm_display_on() && (D.inval || D.inval_all || D.status_dirty)) return true;
    if (nema_obj_poll(pal_input_obj(), NEMA_POLL_IN)) return true;
    nema_port_mutex_lock(D.lock);
    for (int i = 0; i < D.nwatch; i++)
        if (nema_obj_poll(D.watch[i], NEMA_POLL_IN)) {
            nema_port_mutex_unlock(D.lock);
            return true;
        }
    nema_port_mutex_unlock(D.lock);
    return false;
}

static pal_view_t *top_view(void) {
    int n = ovl_count();
    return n ? ovl_view(n - 1) : front();
}

static void notify_sink(psys_notify_change_t change, const psys_notify_t *n) {
    (void)change, (void)n;
    pal_status_changed(); /* the unread count and the toast queue both show here */
}

static bool always(void *ctx) {
    (void)ctx;
    return true;
}

static void desktop_task(void *arg) {
    (void)arg;
    nema_wait_on(NULL, 0, always, NULL, 0); /* creates this task's nema context, which is its TLS pointer */
    desk_tls = nema_port_tls_get();
    const char *first = D.start_view ? D.start_view : D.home;
    if (first[0]) desk_open_now(first);
    if (!front() && D.home[0]) desk_open_now(D.home);
    uint32_t last_tick = now_ms();
    for (;;) {
        uint32_t t = now_ms();
        int32_t wait = 1000; /* the clock needs a look at least once a second */
        pal_view_t *tv = top_view();
        if (tv && tv->tick && tv->tick_ms) {
            int32_t d = (int32_t)(tv->tick_ms - (t - last_tick));
            if (d < wait) wait = d < 0 ? 0 : d;
        }
        int32_t rep = pal_input_next_tick_ms(t);
        if (rep >= 0 && rep < wait) wait = rep;
        int32_t dn = pal_dpm_next_ms(t);
        if (dn >= 0 && dn < wait) wait = dn;
        int32_t tn = pal_toast_next_ms(t);
        if (tn >= 0 && tn < wait) wait = tn;
        int32_t con = pal_console_next_ms(t);
        if (con >= 0 && con < wait) wait = con;
        if (D.chrome && D.chrome->next_ms) {
            int32_t cn = D.chrome->next_ms(t);
            if (cn >= 0 && cn < wait) wait = cn;
        }

        nema_obj_t *objs[MAX_WATCH + 2];
        int n = 0;
        objs[n++] = pal_input_obj();
        objs[n++] = &D.wake;
        nema_port_mutex_lock(D.lock);
        for (int i = 0; i < D.nwatch; i++) objs[n++] = D.watch[i];
        nema_port_mutex_unlock(D.lock);
        nema_wait_on(objs, (size_t)n, pred_work, NULL, wait);

        t = now_ms();
        pal_dpm_tick(t);
        if (pal_toast_service(t)) D.inval_all = true;
        pal_input_tick(t);
        pal_key_t k;
        while (pal_input_pop(&k)) handle_key(&k);
        run_posts();
        pal_console_pump(t);
        for (int i = 0; i < D.nviews; i++)
            if (D.views[i]->pump) {
                D.cur = D.views[i];
                D.views[i]->pump(D.views[i]);
            }
        for (int i = 0; i < ovl_count(); i++)
            if (ovl_view(i)->pump) {
                D.cur = ovl_view(i);
                ovl_view(i)->pump(ovl_view(i));
            }
        tv = top_view();
        if (tv && tv->tick && tv->tick_ms && t - last_tick >= tv->tick_ms) {
            last_tick = t;
            D.cur = tv;
            tv->tick(tv, t);
        }
        if (t - D.last_status_poll >= 1000) { /* battery and radio state change slowly; reading them costs an ADC sample */
            D.last_status_poll = t;
            pal_status_t st = status_now();
            D.polled = st;
            D.polled_ok = true;
            if (!status_same(&st, &D.last_status)) D.status_dirty = true;
        }
        render();
        if (D.chrome && D.chrome->frame && pal_dpm_display_on() && !pal_console_front()) D.chrome->frame(t);
    }
}

void pal_desktop_init(const pal_desktop_cfg_t *cfg) {
    memset(&D, 0, sizeof(D));
    if (cfg) D.cfg = *cfg;
    D.start_view = cfg ? cfg->start_view : NULL;
    D.lock = nema_port_mutex_create();
    D.owner = nema_owner_register("desktop");
    nema_obj_init(&D.wake, &WAKE_OPS);
    D.home[0] = 0; /* no session yet: the session that starts sets its home */
    pal_dpm_hooks_t hooks = {dpm_display, dpm_lock};
    pal_dpm_init(&hooks, now_ms());
    const pal_status_item_t *items = NULL;
    size_t n = pal_status_builtin_items(&items);
    for (size_t i = 0; i < n; i++) pal_status_item_register(&items[i]);
    psys_notify_set_sink(notify_sink);
}

nema_err_t pal_desktop_start(void) {
    if (D.started) return NEMA_ERR_BUSY;
    D.started = true;
    nema_task_desc_t td = {.name = "desktop", .fn = desktop_task, .stack = 8192, .prio = 6, .core = -1,
                           .role = NEMA_TASK_SYSTEM, .owner = D.owner};
    return nema_task_create(&td);
}

uint32_t pal_desktop_frames(void) { return D.frames; }

static const pal_desktop_chrome_t *s_chrome_pending;
static void post_chrome(void *arg) {
    (void)arg;
    D.chrome = s_chrome_pending;
    D.owned = false;
    pal_display_reserve_top(chrome_top());
    D.inval_all = true;
    if (pal_display_canvas()) pal_mark_all(pal_display_canvas());
}

void pal_desktop_set_chrome(const pal_desktop_chrome_t *chrome) {
    s_chrome_pending = chrome;
    run_or_post(post_chrome, NULL);
}

/* The cached reading with the clock brought up to date: the battery is an ADC sample, and a second reader doubled the chance of
 * colliding with a Wi-Fi channel change on hhv2 (interrupt watchdog). */
pal_status_t pal_desktop_status(void) {
    if (!D.polled_ok) return status_now();
    pal_status_t st = D.polled;
    pal_tm_t tm;
    pal_time_local(&tm);
    st.hour = tm.hour;
    st.minute = tm.minute;
    return st;
}
