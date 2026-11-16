#include <string.h>

#include "internal.h"

static struct {
    nema_port_mutex_t *lock;
} W;

struct nema_waiter_node {
    nema_obj_t *obj;
    nema_port_sem_t *sem;
    nema_waiter_node_t *next;
};

void nema_wait_init(void) {
    memset(&W, 0, sizeof(W));
    W.lock = nema_port_mutex_create();
}

/* ---- per-task context ---- */
nema_tctx_t *nema_tctx(void) {
    nema_tctx_t *c = (nema_tctx_t *)nema_port_tls_get();
    if (!c) {
        c = (nema_tctx_t *)nema_alloc_ex(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*c), 0,
                                          NEMA_ALLOC_ZERO | NEMA_ALLOC_CRITICAL);
        NEMA_CHECK(c != NULL);
        c->sem = nema_port_sem_create(0, 1);
        NEMA_CHECK(c->sem != NULL);
        nema_port_tls_set(c);
    }
    return c;
}

void nema_tctx_release(void) {
    nema_tctx_t *c = (nema_tctx_t *)nema_port_tls_get();
    if (!c) return;
    nema_port_tls_set(NULL);
    nema_port_sem_destroy(c->sem);
    nema_free(c);
}

/* ---- objects ---- */
void nema_obj_init(nema_obj_t *o, const nema_obj_ops_t *ops) {
    o->ops = ops;
    o->refs = 1;
    o->waiters = NULL;
    o->flags = 0;
}

/* Reference counting is atomic and lock-free on purpose. Callers routinely hold an object's own lock while
 * taking a reference on its peer; if this needed the wait lock it would invert the lock order against
 * nema_wait_on(), which holds the wait lock while polling objects. */
nema_obj_t *nema_obj_ref(nema_obj_t *o) {
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
    return o;
}

uint32_t nema_obj_poll(nema_obj_t *o, uint32_t events) {
    return o ? o->ops->poll(o, events) : 0;
}

bool nema_obj_try_ref(nema_obj_t *o) {
    int32_t n = __atomic_load_n(&o->refs, __ATOMIC_RELAXED);
    while (n > 0)
        if (__atomic_compare_exchange_n(&o->refs, &n, n + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return true;
    return false;
}

nema_err_t nema_obj_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    return o && o->ops->ioctl ? o->ops->ioctl(o, req, arg) : NEMA_ERR_INVAL;
}

void nema_obj_unref(nema_obj_t *o) {
    if (!o) return;
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0 && o->ops->destroy) o->ops->destroy(o);
}

void nema_notify(nema_obj_t *o) {
    nema_port_mutex_lock(W.lock);
    for (nema_waiter_node_t *n = o->waiters; n; n = n->next) nema_port_sem_give(n->sem);
    nema_port_mutex_unlock(W.lock);
}

static void node_add(nema_obj_t *o, nema_waiter_node_t *n, nema_port_sem_t *sem) {
    n->obj = o;
    n->sem = sem;
    n->next = o->waiters;
    o->waiters = n;
}

static void node_remove(nema_waiter_node_t *n) {
    nema_waiter_node_t **pp = &n->obj->waiters;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
}

/* The one waiting primitive. Register first, then test the predicate, then sleep: a notify that lands after
 * registration signals our semaphore, so no wake-up can be lost between the test and the sleep. */
int nema_wait_on(nema_obj_t *const *objs, size_t nobjs, nema_wait_pred_t pred, void *ctx, int32_t timeout_ms) {
    if (nobjs > NEMA_WAIT_OBJS_MAX) return NEMA_ERR_INVAL;
    nema_tctx_t *tc = nema_tctx();
    nema_proc_t *proc = tc->proc;
    if (proc && proc->killed) nema_cancel_strike();
    nema_waiter_node_t nodes[NEMA_WAIT_OBJS_MAX + 1];
    size_t nn = 0;
    uint64_t deadline = timeout_ms > 0 ? nema_port_time_us() + (uint64_t)timeout_ms * 1000u : 0;
    int result;

    nema_port_mutex_lock(W.lock);
    for (size_t i = 0; i < nobjs; i++) node_add(objs[i], &nodes[nn++], tc->sem);
    if (proc) node_add(&proc->sig_obj, &nodes[nn++], tc->sem);
    for (;;) {
        /* A deferred section (nema_cancel_defer) ignores signals until it ends; then they apply. */
        if (proc && proc->defer == 0) {
            if (proc->killed) {
                result = NEMA_ERR_CANCELED;
                break;
            }
            if (proc->sigpending & NEMA_SIG_FATAL) {
                result = NEMA_ERR_INTR;
                break;
            }
        }
        if (pred(ctx)) {
            result = NEMA_OK;
            break;
        }
        if (timeout_ms == 0) {
            result = NEMA_ERR_TIMEOUT;
            break;
        }
        int32_t wait_ms = -1;
        if (timeout_ms > 0) {
            uint64_t now = nema_port_time_us();
            if (now >= deadline) {
                result = NEMA_ERR_TIMEOUT;
                break;
            }
            wait_ms = (int32_t)((deadline - now + 999) / 1000);
        }
        nema_port_mutex_unlock(W.lock);
        nema_port_sem_take(tc->sem, wait_ms);
        nema_port_mutex_lock(W.lock);
    }
    for (size_t i = 0; i < nn; i++) node_remove(&nodes[i]);
    nema_port_mutex_unlock(W.lock);
    return result;
}

typedef struct {
    nema_obj_t *o;
    uint32_t events;
} ready_ctx_t;

static bool pred_ready(void *p) {
    ready_ctx_t *c = (ready_ctx_t *)p;
    return (c->o->ops->poll(c->o, c->events) & (c->events | NEMA_POLL_HUP | NEMA_POLL_ERR)) != 0;
}

static int64_t deadline_of(int32_t timeout_ms) {
    return timeout_ms > 0 ? (int64_t)nema_port_time_us() + (int64_t)timeout_ms * 1000 : 0;
}

static int32_t remaining_ms(int32_t timeout_ms, int64_t deadline) {
    if (timeout_ms <= 0) return timeout_ms;
    int64_t left = deadline - (int64_t)nema_port_time_us();
    return left <= 0 ? 0 : (int32_t)((left + 999) / 1000);
}

void nema_obj_set_nonblock(nema_obj_t *o, bool on) {
    if (on) __atomic_or_fetch(&o->flags, NEMA_OBJ_NONBLOCK, __ATOMIC_ACQ_REL);
    else __atomic_and_fetch(&o->flags, ~NEMA_OBJ_NONBLOCK, __ATOMIC_ACQ_REL);
}

bool nema_obj_is_nonblock(const nema_obj_t *o) { return (o->flags & NEMA_OBJ_NONBLOCK) != 0; }

int nema_obj_read(nema_obj_t *o, void *buf, size_t n, int32_t timeout_ms) {
    if (nema_canceled()) {
        nema_cancel_strike();
        return NEMA_ERR_CANCELED;
    }
    int64_t dl = deadline_of(timeout_ms);
    for (;;) {
        int r = o->ops->read(o, buf, n);
        if (r != NEMA_ERR_AGAIN || timeout_ms == 0 || (o->flags & NEMA_OBJ_NONBLOCK)) return r;
        ready_ctx_t c = {o, NEMA_POLL_IN};
        int w = nema_wait_on(&o, 1, pred_ready, &c, remaining_ms(timeout_ms, dl));
        if (w != NEMA_OK) return w;
    }
}

static int obj_write_loop(nema_obj_t *o, const void *buf, size_t n, int32_t timeout_ms);

int nema_obj_write(nema_obj_t *o, const void *buf, size_t n, int32_t timeout_ms) {
    if (nema_canceled()) {
        nema_cancel_strike();
        return NEMA_ERR_CANCELED;
    }
    if (!(o->flags & NEMA_OBJ_ATOMIC_WRITE)) return obj_write_loop(o, buf, n, timeout_ms);
    nema_cancel_defer();
    int r = obj_write_loop(o, buf, n, timeout_ms);
    nema_cancel_allow();
    return r;
}

static int obj_write_loop(nema_obj_t *o, const void *buf, size_t n, int32_t timeout_ms) {
    int64_t dl = deadline_of(timeout_ms);
    size_t done = 0;
    while (done < n) {
        int r = o->ops->write(o, (const uint8_t *)buf + done, n - done);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r == 0) break;
        if (r != NEMA_ERR_AGAIN) return done ? (int)done : r;
        if (timeout_ms == 0 || (o->flags & NEMA_OBJ_NONBLOCK)) return done ? (int)done : NEMA_ERR_AGAIN;
        ready_ctx_t c = {o, NEMA_POLL_OUT};
        int w = nema_wait_on(&o, 1, pred_ready, &c, remaining_ms(timeout_ms, dl));
        if (w != NEMA_OK) return done ? (int)done : w;
    }
    return (int)done;
}

typedef struct {
    nema_poll_obj_t *fds;
    size_t n;
    int ready;
} poll_ctx_t;

static bool pred_poll(void *p) {
    poll_ctx_t *c = (poll_ctx_t *)p;
    int ready = 0;
    for (size_t i = 0; i < c->n; i++) {
        nema_obj_t *o = c->fds[i].obj;
        uint32_t ev = c->fds[i].events;
        uint32_t r = o ? o->ops->poll(o, ev) : 0;
        r &= ev | NEMA_POLL_HUP | NEMA_POLL_ERR;
        c->fds[i].revents = r;
        if (r) ready++;
    }
    c->ready = ready;
    return ready > 0;
}

int nema_poll_objs(nema_poll_obj_t *fds, size_t n, int32_t timeout_ms) {
    if (n > NEMA_WAIT_OBJS_MAX) return NEMA_ERR_INVAL;
    nema_obj_t *objs[NEMA_WAIT_OBJS_MAX];
    size_t no = 0;
    for (size_t i = 0; i < n; i++)
        if (fds[i].obj) objs[no++] = fds[i].obj;
    poll_ctx_t c = {fds, n, 0};
    int w = nema_wait_on(objs, no, pred_poll, &c, timeout_ms);
    if (w == NEMA_OK) return c.ready;
    if (w == NEMA_ERR_TIMEOUT) return 0;
    return w;
}

static bool pred_never(void *ctx) {
    (void)ctx;
    return false;
}

int nema_sleep_ms(int32_t ms) {
    int r = nema_wait_on(NULL, 0, pred_never, NULL, ms);
    return r == NEMA_ERR_TIMEOUT ? NEMA_OK : r;
}

/* ---- null device ---- */
static int null_read(nema_obj_t *o, void *b, size_t n) {
    (void)o, (void)b, (void)n;
    return 0;
}
static int null_write(nema_obj_t *o, const void *b, size_t n) {
    (void)o, (void)b;
    return (int)n;
}
static uint32_t null_poll(nema_obj_t *o, uint32_t ev) {
    (void)o, (void)ev;
    return NEMA_POLL_IN | NEMA_POLL_OUT;
}
static nema_err_t null_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    (void)o, (void)req, (void)arg;
    return NEMA_ERR_NOSYS;
}
static const nema_obj_ops_t NULL_OPS = {"null", null_read, null_write, null_poll, null_ioctl, NULL};
static nema_obj_t g_null = {&NULL_OPS, 1, NULL, 0};

nema_obj_t *nema_null_obj(void) { return &g_null; }
