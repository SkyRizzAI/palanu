/* The events module of the palanu API (api/palanu/events.h): pal_wait over several handles, and the process event queue. The
 * queue is a nema object installed as one of the app's handles. It is readable while the process has a pending window change or
 * stop request (RESIZE, QUIT), or while the system has posted events to it (FOCUS, BLUR, EXPOSE, KEY for the app's screen;
 * NOTIFY and SETTING for every app, app_events.h): a wait that includes it wakes exactly for those. The queues are listed, so
 * the system can post from any task without the owner's handle table. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "../src/app_events.h"
#include "nema/nema.h"
#include "nema/port.h"

#define REC_BYTES 8u /* int32 h, uint16 want, uint16 got */
#define REC_MAX NEMA_WAIT_OBJS_MAX
#define EV_BYTES 16u /* uint32 type, a, b, c */
#define EV_RESIZE 2u
#define EV_QUIT 5u
#define RING 8 /* posted events kept per queue; a full ring drops the oldest */

/* The PAL_POLL_* bits of api/palanu/events.h are the NEMA_POLL_* bits: records pass them through unchanged. */
_Static_assert(NEMA_POLL_IN == 0x1 && NEMA_POLL_OUT == 0x2 && NEMA_POLL_HUP == 0x4 && NEMA_POLL_ERR == 0x8,
               "PAL_POLL_* must equal NEMA_POLL_*");

typedef struct ev_queue {
    nema_obj_t obj;    /* first: the handle table holds the reference */
    nema_proc_t *proc; /* the owner: its pending signals make the queue readable */
    bool resize;       /* a window change a wait without the queue saw first: kept here for the next event */
    const void *app;   /* the owner's API context: what psys_events_post_app names */
    struct ev_queue *next;
    uint8_t h, n;
    uint32_t ring[RING][4]; /* type, a, b, c */
} ev_queue_t;

static ev_queue_t *s_queues;
static nema_port_mutex_t *s_qlock;

static void qlock(void) {
    if (!s_qlock) s_qlock = nema_port_mutex_create();
    if (s_qlock) nema_port_mutex_lock(s_qlock);
}
static void qunlock(void) {
    if (s_qlock) nema_port_mutex_unlock(s_qlock);
}

/* Readable while a window change is kept or pending, or a stop request is pending (not while the process defers
 * signals: they apply when the deferral ends). */
static uint32_t ev_poll(nema_obj_t *o, uint32_t events) {
    ev_queue_t *q = (ev_queue_t *)o;
    bool ready = q->resize || q->n > 0;
    if (q->proc && q->proc->defer == 0 && (q->proc->sigpending & (NEMA_SIG_FATAL | NEMA_SIG_WINCH))) ready = true;
    return ready && (events & NEMA_POLL_IN) ? NEMA_POLL_IN : 0;
}

static int ev_read(nema_obj_t *o, void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_PERM; /* events are read with pal_event_next, not as bytes */
}
static int ev_write(nema_obj_t *o, const void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_PERM;
}
static nema_err_t ev_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    (void)o, (void)req, (void)arg;
    return NEMA_ERR_NOSYS;
}
static void ev_destroy(nema_obj_t *o) {
    qlock();
    for (ev_queue_t **pp = &s_queues; *pp; pp = &(*pp)->next)
        if (*pp == (ev_queue_t *)o) {
            *pp = (*pp)->next;
            break;
        }
    qunlock();
    nema_free(o);
}

static const nema_obj_ops_t EV_OPS = {"events", ev_read, ev_write, ev_poll, ev_ioctl, ev_destroy};

/* The app's queue, or NULL when pal_events was never called (or the app closed its handle). */
static ev_queue_t *queue_of(api_ctx_t *c) {
    int32_t fd = api_ext(c)->ev_fd;
    if (fd < 0 || !api_fd_owned(c, fd)) return NULL;
    nema_obj_t *o = nema_handle_obj(fd);
    return o && o->ops == &EV_OPS ? (ev_queue_t *)o : NULL;
}

/* The queue's handle, creating the queue on first use. Negative when the app has no free handle. */
static int32_t queue_open(api_ctx_t *c) {
    if (queue_of(c)) return api_ext(c)->ev_fd;
    if (!api_fd_has_slot(c)) return NEMA_ERR_FULL;
    nema_proc_t *self = nema_self();
    ev_queue_t *q = nema_alloc_ex(NEMA_MEM_ANY, self ? self->owner : NEMA_OWNER_KERNEL, sizeof(*q), 0, NEMA_ALLOC_ZERO);
    if (!q) return NEMA_ERR_NOMEM;
    nema_obj_init(&q->obj, &EV_OPS);
    q->proc = self;
    q->app = c;
    int h = nema_handle_install(&q->obj);
    nema_obj_unref(&q->obj); /* the handle table holds the only reference now (or the object is freed, if it failed) */
    if (h < 0) return h;
    if (!api_fd_add(c, h)) {
        nema_close(h);
        return NEMA_ERR_FULL;
    }
    api_ext(c)->ev_fd = h;
    qlock();
    q->next = s_queues;
    s_queues = q;
    qunlock();
    return h;
}

/* ---- posted events (app_events.h) ---- */

static void post(ev_queue_t *q, uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    if (q->n == RING) q->h = (uint8_t)((q->h + 1) % RING), q->n--; /* full: the oldest goes */
    uint32_t *e = q->ring[(q->h + q->n) % RING];
    e[0] = type, e[1] = a, e[2] = b, e[3] = c;
    q->n++;
}

void psys_events_post_all(uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    qlock();
    for (ev_queue_t *q = s_queues; q; q = q->next) {
        post(q, type, a, b, c);
        nema_notify(&q->obj);
    }
    qunlock();
}

void psys_events_post_app(const void *app, uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    qlock();
    for (ev_queue_t *q = s_queues; q; q = q->next)
        if (q->app == app) {
            post(q, type, a, b, c);
            nema_notify(&q->obj);
        }
    qunlock();
}

uint32_t psys_events_key_hash(const char *key) {
    uint32_t h = 2166136261u;
    for (; key && *key; key++) h = (h ^ (unsigned char)*key) * 16777619u;
    return h;
}

/* Takes the oldest posted event into ev; false when there is none. */
static bool take_posted(ev_queue_t *q, void *ev);

/* A stop request ends the app like a blocking read does; a canceled call (kill) too. */
static int32_t cut(api_ctx_t *c, int32_t n) {
    if (n == NEMA_ERR_CANCELED || n == NEMA_ERR_INTR) api_stop(c, "interrupted");
    return n;
}

/* Milliseconds left for nema_poll: -1 forever, 0 check once, otherwise the time up to deadline. */
static int32_t left_ms(int32_t timeout_ms, int64_t deadline) {
    if (timeout_ms <= 0) return timeout_ms;
    int64_t left = deadline - (int64_t)nema_port_time_us();
    return left <= 0 ? 0 : (int32_t)((left + 999) / 1000);
}

/* A window change that no wait listened for. It is taken from the process (or the wait would return at once, again and
 * again) and kept for the queue, if there is one. */
static void keep_resize(api_ctx_t *c) {
    uint32_t taken = nema_sig_take(~(uint32_t)NEMA_SIG_FATAL); /* every non-fatal bit: WINCH is the only one today */
    ev_queue_t *q = queue_of(c);
    if (q && (taken & NEMA_SIG_WINCH)) q->resize = true;
}

/* A handle the app may wait on: its stdio, or one it holds (an opened file, the event queue). */
static bool handle_ok(api_ctx_t *c, int32_t h) { return (h >= 0 && h <= 2) || api_fd_owned(c, h); }

static void put_event(void *ev, uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    api_put32(ev, type);
    api_put32((uint8_t *)ev + 4, a);
    api_put32((uint8_t *)ev + 8, b);
    api_put32((uint8_t *)ev + 12, c);
}

int32_t psys_api_wait(api_ctx_t *c, void *waitfor, uint32_t n_bytes, int32_t timeout_ms) {
    if (n_bytes % REC_BYTES != 0 || n_bytes / REC_BYTES > REC_MAX) return NEMA_ERR_INVAL;
    uint32_t n = n_bytes / REC_BYTES;
    uint8_t *rec = (uint8_t *)waitfor;
    nema_pollfd_t pf[REC_MAX];
    int32_t qfd = queue_of(c) ? api_ext(c)->ev_fd : -1;
    int qi = -1; /* the record that is the event queue, or -1 */
    for (uint32_t i = 0; i < n; i++) {
        int32_t h = (int32_t)api_get32(rec + i * REC_BYTES);
        uint32_t want = api_get16(rec + i * REC_BYTES + 4);
        if (want & ~0xFu) return NEMA_ERR_INVAL;
        if (!handle_ok(c, h)) return NEMA_ERR_BADF;
        pf[i].fd = h;
        pf[i].events = want & (NEMA_POLL_IN | NEMA_POLL_OUT);
        pf[i].revents = 0;
        if (qfd >= 0 && h == qfd) qi = (int)i;
    }
    int64_t deadline = timeout_ms > 0 ? (int64_t)nema_port_time_us() + (int64_t)timeout_ms * 1000 : 0;
    int r;
    for (;;) {
        r = nema_poll(pf, n, left_ms(timeout_ms, deadline));
        if (r != NEMA_ERR_INTR || nema_interrupted()) break;
        keep_resize(c); /* non-fatal (a window change) and not for this wait: keep it, then wait again */
    }
    if (r == NEMA_ERR_INTR) {
        /* A stop request. If the app waits on the queue, it is ready: the app reads QUIT and decides. Otherwise the app
         * ends here, like a read that is interrupted. */
        if (qi < 0) return cut(c, r);
        for (uint32_t i = 0; i < n; i++) pf[i].revents = 0;
        pf[qi].revents = NEMA_POLL_IN;
        r = 1;
    } else if (r < 0) {
        return cut(c, r);
    }
    for (uint32_t i = 0; i < n; i++) api_put16(rec + i * REC_BYTES + 6, pf[i].revents & (NEMA_POLL_IN | NEMA_POLL_OUT | NEMA_POLL_HUP | NEMA_POLL_ERR));
    return r;
}

int32_t psys_api_events(api_ctx_t *c) { return queue_open(c); }

int32_t psys_api_event_next(api_ctx_t *c, void *ev, uint32_t len, int32_t timeout_ms) {
    if (len < EV_BYTES) return NEMA_ERR_INVAL;
    int32_t fd = queue_open(c);
    if (fd < 0) return fd;
    int64_t deadline = timeout_ms > 0 ? (int64_t)nema_port_time_us() + (int64_t)timeout_ms * 1000 : 0;
    for (;;) {
        if (nema_canceled()) return cut(c, NEMA_ERR_CANCELED);
        if (nema_interrupted()) { /* the stop request is taken here: the app reads QUIT and returns */
            put_event(ev, EV_QUIT, nema_sig_take(NEMA_SIG_FATAL), 0, 0);
            return (int32_t)EV_BYTES;
        }
        ev_queue_t *q = queue_of(c);
        if (!q) return NEMA_ERR_STATE;
        bool winch = nema_sig_take(NEMA_SIG_WINCH) != 0;
        if (winch || q->resize) {
            q->resize = false;
            int cols, rows;
            api_tty_size(&cols, &rows);
            put_event(ev, EV_RESIZE, (uint32_t)cols, (uint32_t)rows, 0);
            return (int32_t)EV_BYTES;
        }
        if (take_posted(q, ev)) return (int32_t)EV_BYTES;
        if (timeout_ms == 0) return 0;
        nema_pollfd_t pf = {fd, NEMA_POLL_IN, 0};
        int r = nema_poll(&pf, 1, left_ms(timeout_ms, deadline));
        if (r == 0) return 0;
        if (r < 0 && r != NEMA_ERR_INTR) return cut(c, r);
        /* ready, or a signal arrived: the loop reads it */
    }
}

static bool take_posted(ev_queue_t *q, void *ev) {
    qlock();
    bool any = q->n > 0;
    if (any) {
        const uint32_t *e = q->ring[q->h];
        put_event(ev, e[0], e[1], e[2], e[3]);
        q->h = (uint8_t)((q->h + 1) % RING);
        q->n--;
    }
    qunlock();
    return any;
}
