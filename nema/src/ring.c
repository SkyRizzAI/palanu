#include <string.h>

#include "nema/ring.h"

size_t nema_ring_write(nema_ring_t *r, const void *data, size_t n) {
    size_t space = r->cap - r->count;
    if (n > space) n = space;
    size_t tail = (r->head + r->count) % r->cap;
    size_t first = r->cap - tail;
    if (first > n) first = n;
    memcpy(r->buf + tail, data, first);
    memcpy(r->buf, (const uint8_t *)data + first, n - first);
    r->count += n;
    return n;
}

size_t nema_ring_read(nema_ring_t *r, void *out, size_t n) {
    if (n > r->count) n = r->count;
    size_t first = r->cap - r->head;
    if (first > n) first = n;
    memcpy(out, r->buf + r->head, first);
    memcpy((uint8_t *)out + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->count -= n;
    return n;
}
