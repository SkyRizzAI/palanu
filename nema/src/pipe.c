#include <string.h>

#include "internal.h"
#include "nema/ring.h"

typedef struct core core_t;
typedef struct {
    nema_obj_t obj;
    core_t *core;
} end_t;

struct core {
    nema_port_mutex_t *lock;
    nema_ring_t ring;
    nema_obj_t *rd, *wr; /* NULL once that end is destroyed */
    end_t rd_end, wr_end;
};

/* ---- read end ---- */
static int rd_read(nema_obj_t *o, void *buf, size_t n) {
    core_t *c = ((end_t *)o)->core;
    nema_port_mutex_lock(c->lock);
    if (nema_ring_count(&c->ring) > 0) {
        size_t got = nema_ring_read(&c->ring, buf, n);
        nema_obj_t *peer = c->wr ? nema_obj_ref(c->wr) : NULL;
        nema_port_mutex_unlock(c->lock);
        if (peer) {
            nema_notify(peer);
            nema_obj_unref(peer);
        }
        return (int)got;
    }
    bool eof = c->wr == NULL;
    nema_port_mutex_unlock(c->lock);
    return eof ? 0 : NEMA_ERR_AGAIN;
}

static uint32_t rd_poll(nema_obj_t *o, uint32_t ev) {
    core_t *c = ((end_t *)o)->core;
    uint32_t r = 0;
    nema_port_mutex_lock(c->lock);
    bool data = nema_ring_count(&c->ring) > 0;
    bool eof = c->wr == NULL;
    nema_port_mutex_unlock(c->lock);
    if (data || eof) r |= NEMA_POLL_IN;
    if (eof && !data) r |= NEMA_POLL_HUP;
    (void)ev;
    return r;
}

/* ---- write end ---- */
static int wr_write(nema_obj_t *o, const void *buf, size_t n) {
    core_t *c = ((end_t *)o)->core;
    nema_port_mutex_lock(c->lock);
    if (c->rd == NULL) {
        nema_port_mutex_unlock(c->lock);
        return NEMA_ERR_PIPE;
    }
    size_t put = nema_ring_write(&c->ring, buf, n);
    nema_obj_t *peer = put ? nema_obj_ref(c->rd) : NULL;
    nema_port_mutex_unlock(c->lock);
    if (peer) {
        nema_notify(peer);
        nema_obj_unref(peer);
    }
    return put ? (int)put : NEMA_ERR_AGAIN;
}

static uint32_t wr_poll(nema_obj_t *o, uint32_t ev) {
    core_t *c = ((end_t *)o)->core;
    uint32_t r = 0;
    nema_port_mutex_lock(c->lock);
    bool broken = c->rd == NULL;
    bool space = nema_ring_space(&c->ring) > 0;
    nema_port_mutex_unlock(c->lock);
    if (space || broken) r |= NEMA_POLL_OUT;
    if (broken) r |= NEMA_POLL_ERR | NEMA_POLL_HUP;
    (void)ev;
    return r;
}

static int bad_rw_read(nema_obj_t *o, void *b, size_t n) {
    (void)o, (void)b, (void)n;
    return NEMA_ERR_PERM;
}
static int bad_rw_write(nema_obj_t *o, const void *b, size_t n) {
    (void)o, (void)b, (void)n;
    return NEMA_ERR_PERM;
}
static nema_err_t pipe_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    (void)o, (void)req, (void)arg;
    return NEMA_ERR_NOSYS;
}

/* Destroying an end marks it gone and wakes the other side. The block is freed by whichever end goes last. */
static void end_destroy(nema_obj_t *o, bool reader) {
    core_t *c = ((end_t *)o)->core;
    nema_port_mutex_lock(c->lock);
    nema_obj_t *peer;
    if (reader) {
        c->rd = NULL;
        peer = c->wr ? nema_obj_ref(c->wr) : NULL;
    } else {
        c->wr = NULL;
        peer = c->rd ? nema_obj_ref(c->rd) : NULL;
    }
    bool last = c->rd == NULL && c->wr == NULL;
    nema_port_mutex_unlock(c->lock);
    if (peer) {
        nema_notify(peer);
        nema_obj_unref(peer);
    }
    if (last) {
        nema_port_mutex_destroy(c->lock);
        nema_free(c);
    }
}
static void rd_destroy(nema_obj_t *o) { end_destroy(o, true); }
static void wr_destroy(nema_obj_t *o) { end_destroy(o, false); }

static const nema_obj_ops_t RD_OPS = {"pipe-r", rd_read, bad_rw_write, rd_poll, pipe_ioctl, rd_destroy};
static const nema_obj_ops_t WR_OPS = {"pipe-w", bad_rw_read, wr_write, wr_poll, pipe_ioctl, wr_destroy};

nema_err_t nema_pipe_create(size_t capacity, nema_mem_class_t cls, nema_owner_t owner, nema_obj_t **rd,
                            nema_obj_t **wr) {
    if (!capacity || !rd || !wr) return NEMA_ERR_INVAL;
    /* The core is shared by reference count across processes: never process-owned (the exit sweep would free it). */
    core_t *c = (core_t *)nema_alloc_ex(cls, nema_mem_shared_owner(owner), sizeof(core_t) + capacity, 0, NEMA_ALLOC_ZERO);
    if (!c) return NEMA_ERR_NOMEM;
    c->lock = nema_port_mutex_create();
    if (!c->lock) {
        nema_free(c);
        return NEMA_ERR_NOMEM;
    }
    nema_ring_init(&c->ring, (uint8_t *)(c + 1), capacity);
    c->rd_end.core = c->wr_end.core = c;
    nema_obj_init(&c->rd_end.obj, &RD_OPS);
    nema_obj_init(&c->wr_end.obj, &WR_OPS);
    c->rd = &c->rd_end.obj;
    c->wr = &c->wr_end.obj;
    *rd = c->rd;
    *wr = c->wr;
    return NEMA_OK;
}
