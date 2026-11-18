/* App screens (api/palanu/ui.h, ADR-0010 §8): a form or a canvas for an app that is not a terminal app. Two slots, one per app.
 *
 * A form is a menu page made of the app's fields and bound to the slot's view (pal_menu_bind): the classic session draws it with the
 * theme's list, Athena as its grouped list, so the app looks native under each. The rows' callbacks run on the desktop task: they
 * change the field and queue an event the app reads with pal_form_next. A canvas is an RGB565 buffer the size of the view's area;
 * after the compositor's frame the front canvas is copied into the screen frame (appui_after_render), and its keys are queued for
 * pal_canvas_key. The app's calls run on its own task: they take the slot's lock and wake the desktop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appui.h"
#include "desktop_int.h"
#include "nema/nema.h"
#include "pal/display.h"
#include "pal/menu.h"
#include "pal/theme.h"

#define SLOTS 2
#define FIELDS 32
#define EVENTS 16
#define CHOICES 8

enum { K_SECTION = 1, K_INFO, K_TOGGLE, K_CHOICE, K_SLIDER, K_ACTION, K_TEXT }; /* PAL_FORM_* */
enum { EV_CHANGED = 1, EV_ACTIVATED, EV_CLOSED };

typedef struct slot slot_t;

typedef struct {
    slot_t *s;
    int id, kind, value, min, max, step;
    char label[32];
    char text[96];       /* INFO and TEXT: the value; CHOICE: the names, '|' replaced by NULs */
    const char *names[CHOICES + 1];
} field_t;

typedef struct {
    int32_t a, b, c; /* form: id, kind, value; canvas: sym | mods << 8, code point, 0 */
} event_t;

struct slot {
    bool used, open, is_canvas, closed_flag;
    const void *app; /* the app's api context: who the slot is for */
    char vid[16], title[32];
    pal_view_t view;
    nema_obj_t obj;
    nema_port_mutex_t *lock;
    /* form */
    pal_menu_t menu;
    pal_menu_page_t page;
    pal_menu_row_t rows[FIELDS];
    field_t f[FIELDS];
    int nf;
    volatile bool dirty;
    /* events (form) or keys (canvas) */
    event_t ev[EVENTS];
    int eh, en;
    /* canvas */
    uint16_t *px;
    int w, h;
    pal_rect_t pend; /* what changed since the last copy, in canvas coordinates; w 0 = nothing */
    bool full;
};

static slot_t S[SLOTS] NEMA_EXT_BSS;
static nema_owner_t s_owner;

static uint32_t o_poll(nema_obj_t *o, uint32_t e) {
    (void)e;
    for (int i = 0; i < SLOTS; i++)
        if (&S[i].obj == o) return S[i].en > 0 || S[i].closed_flag ? NEMA_POLL_IN : 0;
    return 0;
}
static int o_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static int o_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static nema_err_t o_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t OPS = {"appui", o_read, o_write, o_poll, o_ioctl, NULL};

static void push(slot_t *s, int32_t a, int32_t b, int32_t c) {
    nema_port_mutex_lock(s->lock);
    if (s->en < EVENTS) s->ev[(s->eh + s->en++) % EVENTS] = (event_t){a, b, c};
    nema_port_mutex_unlock(s->lock);
    nema_notify(&s->obj);
}

/* ---- the form's rows ---- */

static int f_get(void *ctx) { return ((field_t *)ctx)->value; }
static void f_set(void *ctx, int v) {
    field_t *f = ctx;
    f->value = v;
    push(f->s, f->id, EV_CHANGED, v);
}
static void f_run(void *ctx) {
    field_t *f = ctx;
    push(f->s, f->id, EV_ACTIVATED, 0);
}
static void f_text(void *ctx, char *out, size_t cap) { nema_strlcpy(out, ((field_t *)ctx)->text, cap); }
static bool f_apply(void *ctx, const char *t) {
    field_t *f = ctx;
    nema_strlcpy(f->text, t, 64);
    push(f->s, f->id, EV_CHANGED, 0);
    return true;
}

static void build(pal_menu_page_t *p) {
    slot_t *s = p->ctx;
    pal_menu_clear(p);
    nema_port_mutex_lock(s->lock);
    for (int i = 0; i < s->nf; i++) {
        field_t *f = &s->f[i];
        pal_menu_row_t r = {.label = f->label, .ctx = f};
        switch (f->kind) {
        case K_SECTION: r.kind = PAL_MENU_SECTION; break;
        case K_INFO: r.kind = PAL_MENU_INFO, r.text = f_text, r.inert = true; break;
        case K_TOGGLE: r.kind = PAL_MENU_TOGGLE, r.get = f_get, r.set = f_set; break;
        case K_CHOICE: r.kind = PAL_MENU_CHOICE, r.get = f_get, r.set = f_set, r.choices = f->names; break;
        case K_SLIDER: r.kind = PAL_MENU_SLIDER, r.get = f_get, r.set = f_set, r.min = f->min, r.max = f->max, r.step = f->step; break;
        case K_ACTION: r.kind = PAL_MENU_ACTION, r.run = f_run; break;
        case K_TEXT: r.kind = PAL_MENU_TEXT, r.text = f_text, r.apply_text = f_apply, r.max_len = 63; break;
        default: continue;
        }
        pal_menu_add(p, &r);
    }
    s->dirty = false;
    nema_port_mutex_unlock(s->lock);
}

/* ---- the views ---- */

static void v_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    slot_t *s = v->data;
    if (!s->is_canvas) {
        pal_menu_draw(&s->menu, c, a, pal_theme_active());
        return;
    }
    (void)full;
    s->full = true; /* the compositor painted the area: the canvas goes over it again after the frame */
}

static bool v_key(pal_view_t *v, const pal_key_t *k) {
    slot_t *s = v->data;
    if (s->is_canvas) {
        push(s, (int32_t)(k->sym | (k->mods << 8)), (int32_t)k->cp, 0);
        return true; /* every key is the app's; the home key and the session's keys never reach here */
    }
    bool used = pal_menu_key(&s->menu, k);
    if (used) {
        pal_menu_follow(&s->menu);
        pal_desktop_invalidate();
    }
    return used; /* Esc at the root is not used: the session takes the person home */
}

static void v_enter(pal_view_t *v) {
    slot_t *s = v->data;
    s->closed_flag = false;
    if (!s->is_canvas) pal_menu_bind(s->vid, &s->menu); /* the session may draw it in its own look */
    s->full = true;
}

static void v_leave(pal_view_t *v) {
    slot_t *s = v->data;
    if (!s->used) return;
    s->closed_flag = true;
    if (!s->is_canvas) push(s, 0, EV_CLOSED, 0);
    else nema_notify(&s->obj);
}

static void v_pump(pal_view_t *v) {
    slot_t *s = v->data;
    if (!s->used || s->is_canvas || !s->dirty) return;
    pal_menu_refresh(&s->menu);
    pal_desktop_invalidate();
}

void appui_init(void) {
    if (!s_owner) s_owner = nema_owner_register("appui");
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &S[i];
        if (s->lock) continue;
        s->lock = nema_port_mutex_create();
        nema_obj_init(&s->obj, &OPS);
        snprintf(s->vid, sizeof(s->vid), "app-screen-%d", i);
        s->view = (pal_view_t){.id = s->vid, .title = s->title, .flags = PAL_VIEW_HIDDEN, .enter = v_enter, .leave = v_leave,
                               .draw = v_draw, .key = v_key, .pump = v_pump, .data = s};
        s->page = (pal_menu_page_t){.title = s->title, .store = s->rows, .cap = FIELDS, .build = build, .ctx = s};
        pal_menu_init(&s->menu, &s->page);
        (void)pal_desktop_register(&s->view);
    }
}

/* ---- for the API (system/api/ui.c) ---- */

static slot_t *find(const void *app) {
    for (int i = 0; i < SLOTS; i++)
        if (S[i].used && S[i].app == app) return &S[i];
    return NULL;
}

static slot_t *take(const void *app, bool canvas, const char *title, nema_err_t *err) {
    slot_t *s = find(app);
    if (s && s->is_canvas != canvas) {
        *err = NEMA_ERR_BUSY;
        return NULL;
    }
    if (!s) {
        desk_lock();
        for (int i = 0; i < SLOTS && !s; i++)
            if (!S[i].used && S[i].lock) s = &S[i];
        if (s) {
            s->used = true;
            s->app = app;
            s->is_canvas = canvas;
            s->nf = s->eh = s->en = 0;
            s->closed_flag = false;
        }
        desk_unlock();
        if (!s) {
            *err = S[0].lock ? NEMA_ERR_BUSY : NEMA_ERR_UNSUPPORTED;
            return NULL;
        }
    }
    nema_strlcpy(s->title, title ? title : "", sizeof(s->title));
    *err = NEMA_OK;
    return s;
}

int32_t appui_form_open(const void *app, const char *title) {
    nema_err_t err;
    slot_t *s = take(app, false, title, &err);
    if (!s) return err;
    s->dirty = true;
    pal_desktop_open(s->vid);
    pal_desktop_invalidate();
    return 0;
}

static field_t *field(slot_t *s, int id) {
    for (int i = 0; i < s->nf; i++)
        if (s->f[i].id == id) return &s->f[i];
    return NULL;
}

/* "a|b|c" into the field's text, the names pointing into it. */
static int split(field_t *f, const char *v) {
    nema_strlcpy(f->text, v, sizeof(f->text));
    int n = 0;
    char *p = f->text;
    while (n < CHOICES) {
        f->names[n++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = 0;
        p = bar + 1;
    }
    f->names[n] = NULL;
    return n;
}

static bool set_value(field_t *f, const char *v) {
    switch (f->kind) {
    case K_INFO:
    case K_TEXT: nema_strlcpy(f->text, v, f->kind == K_TEXT ? 64 : sizeof(f->text)); return true;
    case K_TOGGLE: f->value = atoi(v) != 0; return true;
    case K_CHOICE: {
        int i = atoi(v), n = 0;
        while (f->names[n]) n++;
        if (i < 0 || i >= n) return false;
        f->value = i;
        return true;
    }
    case K_SLIDER: {
        int x = atoi(v);
        f->value = x < f->min ? f->min : x > f->max ? f->max : x;
        return true;
    }
    default: return false;
    }
}

int32_t appui_form_add(const void *app, int32_t id, int32_t kind, const char *label, const char *value) {
    slot_t *s = find(app);
    if (!s || s->is_canvas) return NEMA_ERR_NOENT;
    if (id < 0 || id > 65535 || kind < K_SECTION || kind > K_TEXT || !label) return NEMA_ERR_INVAL;
    if (!value) value = "";
    nema_port_mutex_lock(s->lock);
    nema_err_t r = NEMA_OK;
    if (field(s, id)) r = NEMA_ERR_EXIST;
    else if (s->nf == FIELDS) r = NEMA_ERR_FULL;
    if (r == NEMA_OK) {
        field_t *f = &s->f[s->nf];
        memset(f, 0, sizeof(*f));
        f->s = s, f->id = id, f->kind = kind;
        nema_strlcpy(f->label, label, sizeof(f->label));
        if (kind == K_CHOICE) split(f, value);
        else if (kind == K_SLIDER) {
            int a = 0, b = 100, st = 1, v = 0;
            if (sscanf(value, "%d|%d|%d|%d", &a, &b, &st, &v) < 2 || b < a) r = NEMA_ERR_INVAL;
            f->min = a, f->max = b, f->step = st > 0 ? st : 1, f->value = v < a ? a : v > b ? b : v;
        } else if (kind != K_SECTION && kind != K_ACTION) set_value(f, value);
        if (r == NEMA_OK) s->nf++;
    }
    s->dirty = true;
    nema_port_mutex_unlock(s->lock);
    pal_desktop_invalidate();
    return r;
}

int32_t appui_form_set(const void *app, int32_t id, const char *value) {
    slot_t *s = find(app);
    if (!s || s->is_canvas) return NEMA_ERR_NOENT;
    nema_port_mutex_lock(s->lock);
    field_t *f = field(s, id);
    nema_err_t r = !f ? NEMA_ERR_NOENT : set_value(f, value ? value : "") ? NEMA_OK : NEMA_ERR_INVAL;
    s->dirty = true;
    nema_port_mutex_unlock(s->lock);
    pal_desktop_invalidate();
    return r;
}

int32_t appui_form_get(const void *app, int32_t id, char *buf, uint32_t len) {
    slot_t *s = find(app);
    if (!s || s->is_canvas) return NEMA_ERR_NOENT;
    char t[96];
    nema_port_mutex_lock(s->lock);
    field_t *f = field(s, id);
    if (f) {
        if (f->kind == K_INFO || f->kind == K_TEXT) nema_strlcpy(t, f->text, sizeof(t));
        else if (f->kind == K_CHOICE) nema_strlcpy(t, f->names[f->value] ? f->names[f->value] : "", sizeof(t));
        else snprintf(t, sizeof(t), "%d", f->value);
    }
    nema_port_mutex_unlock(s->lock);
    if (!f) return NEMA_ERR_NOENT;
    size_t n = strlen(t);
    if (!buf || n + 1 > len) return NEMA_ERR_INVAL;
    memcpy(buf, t, n + 1);
    return (int32_t)n;
}

int32_t appui_form_clear(const void *app) {
    slot_t *s = find(app);
    if (!s || s->is_canvas) return NEMA_ERR_NOENT;
    nema_port_mutex_lock(s->lock);
    s->nf = 0;
    s->dirty = true;
    nema_port_mutex_unlock(s->lock);
    pal_desktop_invalidate();
    return 0;
}

static bool ready(void *ctx) {
    slot_t *s = ctx;
    return s->en > 0 || (s->is_canvas && s->closed_flag);
}

/* The next event or key: 1 and the record, 0 on timeout, 2 when a canvas was left. */
static int32_t next(slot_t *s, event_t *out, int32_t timeout_ms) {
    nema_obj_t *o = &s->obj;
    int w = nema_wait_on(&o, 1, ready, s, timeout_ms);
    if (w == NEMA_ERR_INTR) return NEMA_ERR_INTR;
    nema_port_mutex_lock(s->lock);
    int32_t r = 0;
    if (s->en > 0) {
        *out = s->ev[s->eh];
        s->eh = (s->eh + 1) % EVENTS;
        s->en--;
        r = 1;
    } else if (s->is_canvas && s->closed_flag) {
        s->closed_flag = false;
        r = 2;
    }
    nema_port_mutex_unlock(s->lock);
    return r;
}

int32_t appui_form_next(const void *app, int32_t *rec, int32_t timeout_ms) {
    slot_t *s = find(app);
    if (!s || s->is_canvas) return NEMA_ERR_NOENT;
    event_t e;
    int32_t r = next(s, &e, timeout_ms);
    if (r == 1) rec[0] = e.a, rec[1] = e.b, rec[2] = e.c;
    return r;
}

static void release(slot_t *s) {
    bool front = !strcmp(pal_desktop_front_id(), s->vid);
    s->used = false;
    if (front) pal_app_exit();
    nema_free(s->px);
    s->px = NULL;
    s->app = NULL;
}

int32_t appui_close(const void *app, bool canvas) {
    slot_t *s = find(app);
    if (!s || s->is_canvas != canvas) return NEMA_ERR_NOENT;
    release(s);
    return 0;
}

int32_t appui_canvas_open(const void *app, const char *title) {
    if (!pal_display_frame()) return NEMA_ERR_UNSUPPORTED;
    nema_err_t err;
    slot_t *s = take(app, true, title, &err);
    if (!s) return err;
    pal_rect_t a = pal_desktop_area(&s->view);
    if (!s->px || s->w != a.w || s->h != a.h) {
        nema_free(s->px);
        s->w = a.w, s->h = a.h;
        s->px = nema_alloc_ex(NEMA_MEM_EXTERNAL, s_owner, (size_t)a.w * (size_t)a.h * 2u, 4, NEMA_ALLOC_ZERO);
        if (!s->px) s->px = nema_alloc_ex(NEMA_MEM_ANY, s_owner, (size_t)a.w * (size_t)a.h * 2u, 4, NEMA_ALLOC_ZERO);
        if (!s->px) {
            s->used = false;
            return NEMA_ERR_NOMEM;
        }
    }
    s->full = true;
    pal_desktop_open(s->vid);
    pal_desktop_invalidate();
    return (s->w << 16) | s->h;
}

int32_t appui_canvas_present(const void *app, const uint8_t *px, uint32_t len, int32_t x, int32_t y, int32_t w) {
    slot_t *s = find(app);
    if (!s || !s->is_canvas) return NEMA_ERR_NOENT;
    if (w <= 0 || len % (uint32_t)(2 * w)) return NEMA_ERR_INVAL;
    int h = (int)(len / (uint32_t)(2 * w));
    if (x < 0 || y < 0 || x + w > s->w || y + h > s->h) return NEMA_ERR_INVAL;
    nema_port_mutex_lock(s->lock);
    for (int r = 0; r < h; r++) {
        uint16_t *dst = s->px + (size_t)(y + r) * (size_t)s->w + (size_t)x;
        const uint8_t *src = px + (size_t)r * (size_t)w * 2u;
        for (int i = 0; i < w; i++) dst[i] = (uint16_t)(src[2 * i] | (src[2 * i + 1] << 8));
    }
    if (s->pend.w == 0) s->pend = (pal_rect_t){x, y, w, h};
    else {
        int x1 = s->pend.x + s->pend.w > x + w ? s->pend.x + s->pend.w : x + w;
        int y1 = s->pend.y + s->pend.h > y + h ? s->pend.y + s->pend.h : y + h;
        s->pend.x = s->pend.x < x ? s->pend.x : x, s->pend.y = s->pend.y < y ? s->pend.y : y;
        s->pend.w = x1 - s->pend.x, s->pend.h = y1 - s->pend.y;
    }
    nema_port_mutex_unlock(s->lock);
    pal_desktop_invalidate();
    return 0;
}

int32_t appui_canvas_key(const void *app, uint8_t *rec, int32_t timeout_ms) {
    slot_t *s = find(app);
    if (!s || !s->is_canvas) return NEMA_ERR_NOENT;
    event_t e;
    int32_t r = next(s, &e, timeout_ms);
    if (r == 1) {
        rec[0] = (uint8_t)e.a, rec[1] = (uint8_t)(e.a >> 8), rec[2] = rec[3] = 0;
        uint32_t cp = (uint32_t)e.b;
        rec[4] = (uint8_t)cp, rec[5] = (uint8_t)(cp >> 8), rec[6] = (uint8_t)(cp >> 16), rec[7] = (uint8_t)(cp >> 24);
    }
    return r;
}

/* After the compositor's frame (desktop task): the front canvas, when nothing covers it, over its area of the screen frame. */
void appui_after_render(void) {
    for (int i = 0; i < SLOTS; i++) {
        slot_t *s = &S[i];
        if (!s->used || !s->is_canvas || !s->px || ovl_count() || strcmp(pal_desktop_front_id(), s->vid)) continue;
        pal_rect_t a = pal_desktop_area(&s->view);
        nema_port_mutex_lock(s->lock);
        pal_rect_t r = s->full ? (pal_rect_t){0, 0, s->w, s->h} : s->pend;
        s->full = false;
        s->pend.w = 0;
        if (r.w > 0 && r.h > 0) {
            if (r.x + r.w > a.w) r.w = a.w - r.x;
            if (r.y + r.h > a.h) r.h = a.h - r.y;
            pal_display_blit_rgb565(s->px + (size_t)r.y * (size_t)s->w + (size_t)r.x, s->w, (pal_rect_t){a.x + r.x, a.y + r.y, r.w, r.h});
        }
        nema_port_mutex_unlock(s->lock);
    }
}

/* The app ended: its screen goes with it. */
void appui_release(const void *app) {
    slot_t *s = find(app);
    if (s) release(s);
}
