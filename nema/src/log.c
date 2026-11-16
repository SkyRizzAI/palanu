#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nema/log.h"
#include "nema/mem.h"

static struct {
    nema_port_mutex_t *lock;
    char *ring;
    size_t cap, head, used;
    bool console;
    nema_log_level_t max_level;
} L;

void nema_log_init(size_t ring_bytes, bool console);
void nema_log_init(size_t ring_bytes, bool console) {
    memset(&L, 0, sizeof(L));
    L.lock = nema_port_mutex_create();
    L.cap = ring_bytes ? ring_bytes : 16 * 1024;
    L.ring = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, L.cap);
    if (!L.ring) L.cap = 0;
    L.console = console;
    L.max_level = NEMA_LOG_DEBUG;
}

void nema_log_set_console(bool on) { L.console = on; }
void nema_log_set_level(nema_log_level_t max_level) { L.max_level = max_level; }

static void ring_put(const char *s, size_t n) {
    if (!L.cap) return;
    for (size_t i = 0; i < n; i++) {
        L.ring[L.head] = s[i];
        L.head = (L.head + 1) % L.cap;
    }
    L.used += n;
    if (L.used > L.cap) L.used = L.cap;
}

void nema_log(nema_log_level_t level, const char *tag, const char *fmt, ...) {
    if (level > L.max_level || !L.lock) return;
    static const char letters[] = "EWID";
    char line[192];
    int n = snprintf(line, sizeof(line), "%c (%lu) %s: ", letters[level],
                     (unsigned long)(nema_port_time_us() / 1000), tag);
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
    if (len > sizeof(line) - 3) len = sizeof(line) - 3;
    line[len++] = '\n';
    nema_port_mutex_lock(L.lock);
    ring_put(line, len);
    bool console = L.console;
    nema_port_mutex_unlock(L.lock);
    if (console) {
        line[len - 1] = '\r';
        line[len++] = '\n';
        nema_port_console_write(line, len);
    }
}

size_t nema_log_snapshot(char *dst, size_t cap) {
    if (!cap) return 0;
    nema_port_mutex_lock(L.lock);
    size_t n = L.used < cap - 1 ? L.used : cap - 1;
    size_t start = (L.head + L.cap - n) % L.cap;
    for (size_t i = 0; i < n; i++) dst[i] = L.ring[(start + i) % L.cap];
    nema_port_mutex_unlock(L.lock);
    /* drop a leading partial line */
    size_t skip = 0;
    if (n == cap - 1 && L.used >= cap - 1) {
        while (skip < n && dst[skip] != '\n') skip++;
        if (skip < n) skip++;
        else skip = 0;
    }
    if (skip) memmove(dst, dst + skip, n - skip);
    n -= skip;
    dst[n] = 0;
    return n;
}
