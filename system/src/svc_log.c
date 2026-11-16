/* The log of a service: a ring of PSYS_SVC_LOG_BYTES (PSRAM) that keeps the newest bytes. Each complete line also goes to
 * the nema log, tagged with the service id. The ring is written by the supervisor only; readers take the lock. */

#include <string.h>

#include "svc_int.h"

void svc_ensure_log(svc_t *e) {
    if (e->log) return;
    e->log = (uint8_t *)nema_alloc(NEMA_MEM_EXTERNAL, svc_g->owner, PSYS_SVC_LOG_BYTES);
    if (!e->log) NEMA_LOGW("svc", "%s: no memory for its log; output is only in the nema log", e->id);
}
void svc_log_line(svc_t *e) {
    e->line[e->linelen] = 0;
    if (e->linelen && e->line[e->linelen - 1] == '\r') e->line[e->linelen - 1] = 0;
    NEMA_LOGI(e->id, "%s", e->line);
    e->linelen = 0;
}
void svc_log_bytes(svc_t *e, const char *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (e->log) e->log[e->log_total % PSYS_SVC_LOG_BYTES] = (uint8_t)d[i];
        e->log_total++;
        if (d[i] == '\n') {
            svc_log_line(e);
        } else if (e->linelen < LINE_MAX - 1) {
            e->line[e->linelen++] = d[i];
        }
    }
    nema_notify(&e->log_obj);
}
/* Reads what the process has written so far. Lock held. Sets rd_eof when the process and its children are gone. */
void svc_drain(svc_t *e) {
    if (!e->rd || e->rd_eof) return;
    char b[DRAIN_CHUNK];
    for (;;) {
        int n = nema_obj_read(e->rd, b, sizeof(b), 0);
        if (n > 0) {
            svc_log_bytes(e, b, (size_t)n);
            continue;
        }
        if (n == 0) e->rd_eof = true;
        return;
    }
}
void svc_close_rd(svc_t *e) {
    svc_drain(e);
    if (e->linelen) svc_log_line(e);
    if (e->rd) nema_obj_unref(e->rd);
    e->rd = NULL;
    e->rd_eof = false;
}
int psys_svc_log_read(const char *id, uint64_t *cursor, char *buf, size_t cap) {
    svc_ensure_init();
    svc_lock();
    svc_t *e = svc_find_or_ensure(id);
    if (!e) {
        svc_unlock();
        return NEMA_ERR_NOENT;
    }
    uint64_t total = e->log_total;
    uint64_t oldest = total > PSYS_SVC_LOG_BYTES ? total - PSYS_SVC_LOG_BYTES : 0;
    uint64_t at = *cursor < oldest ? oldest : *cursor;
    size_t n = 0;
    if (e->log) {
        while (at < total && n < cap) {
            buf[n++] = (char)e->log[at % PSYS_SVC_LOG_BYTES];
            at++;
        }
    }
    *cursor = at;
    svc_unlock();
    return (int)n;
}
typedef struct {
    const svc_t *e;
    uint64_t cursor;
} log_wait_t;

static bool pred_log(void *ctx) {
    const log_wait_t *w = (const log_wait_t *)ctx;
    return w->e->log_total > w->cursor;
}
nema_err_t psys_svc_log_wait(const char *id, uint64_t cursor, int32_t timeout_ms) {
    svc_ensure_init();
    svc_lock();
    svc_t *e = svc_find_or_ensure(id);
    svc_unlock();
    if (!e) return NEMA_ERR_NOENT;
    log_wait_t w = {e, cursor};
    nema_obj_t *o = &e->log_obj;
    return nema_wait_on(&o, 1, pred_log, &w, timeout_ms);
}
