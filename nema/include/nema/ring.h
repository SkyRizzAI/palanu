/* Nema ring: a byte ring buffer. Not thread-safe by itself; owners lock around it. */
#ifndef NEMA_RING_H
#define NEMA_RING_H

#include "nema/types.h"

typedef struct {
    uint8_t *buf;
    size_t cap, head, count;
} nema_ring_t;

static inline void nema_ring_init(nema_ring_t *r, void *buf, size_t cap) {
    r->buf = (uint8_t *)buf;
    r->cap = cap;
    r->head = 0;
    r->count = 0;
}
static inline size_t nema_ring_count(const nema_ring_t *r) { return r->count; }
static inline size_t nema_ring_space(const nema_ring_t *r) { return r->cap - r->count; }
size_t nema_ring_write(nema_ring_t *r, const void *data, size_t n); /* returns bytes accepted */
size_t nema_ring_read(nema_ring_t *r, void *out, size_t n);         /* returns bytes produced */
static inline void nema_ring_clear(nema_ring_t *r) {
    r->head = 0;
    r->count = 0;
}

#endif
