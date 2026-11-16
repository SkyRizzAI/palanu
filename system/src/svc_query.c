/* Read-only views of the service table for the shell and the API. */

#include <stdio.h>
#include <string.h>

#include "svc_int.h"

void svc_fill(const svc_t *e, psys_svc_info_t *out) {
    memset(out, 0, sizeof(*out));
    svc_setstr(out->id, sizeof(out->id), e->id);
    svc_setstr(out->name, sizeof(out->name), e->name);
    svc_setstr(out->reason, sizeof(out->reason), e->reason);
    out->state = e->state;
    out->enabled = e->enabled;
    out->core = e->bi.core;
    out->installed = e->installed;
    out->pid = e->pid;
    out->restarts = e->restarts;
    out->failures = e->failures;
    out->started_us = e->started_us;
    out->log_total = e->log_total;
}
size_t psys_svc_count(void) {
    svc_ensure_init();
    svc_lock();
    svc_sync_installed();
    size_t n = 0;
    for (size_t i = 0; i < PSYS_SVC_MAX; i++)
        if (svc_visible(&svc_g->t[i])) n++;
    svc_unlock();
    return n;
}
bool psys_svc_info(size_t i, psys_svc_info_t *out) {
    svc_ensure_init();
    svc_lock();
    svc_sync_installed();
    size_t k = 0;
    bool ok = false;
    for (size_t j = 0; j < PSYS_SVC_MAX && !ok; j++) {
        if (!svc_visible(&svc_g->t[j])) continue;
        if (k++ == i) {
            svc_fill(&svc_g->t[j], out);
            ok = true;
        }
    }
    svc_unlock();
    return ok;
}
bool psys_svc_find(const char *id, psys_svc_info_t *out) {
    svc_ensure_init();
    svc_lock();
    svc_t *e = svc_find_or_ensure(id);
    if (e) svc_fill(e, out);
    svc_unlock();
    return e != NULL;
}
const char *psys_svc_state_name(psys_svc_state_t s) {
    switch (s) {
    case PSYS_SVC_STOPPED: return "stopped";
    case PSYS_SVC_WAITING_MEMORY: return "waiting-memory";
    case PSYS_SVC_STARTING: return "starting";
    case PSYS_SVC_RUNNING: return "running";
    case PSYS_SVC_FAILED: return "failed";
    default: return "?";
    }
}
const char *psys_svc_restart_name(psys_svc_restart_t r) {
    switch (r) {
    case PSYS_SVC_RESTART_NEVER: return "never";
    case PSYS_SVC_RESTART_ON_FAILURE: return "on-failure";
    case PSYS_SVC_RESTART_ALWAYS: return "always";
    default: return "?";
    }
}
bool psys_svc_settled(void) {
    svc_ensure_init();
    svc_lock();
    bool settled = true;
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        const svc_t *e = &svc_g->t[i];
        if (e->used && (e->state == PSYS_SVC_STARTING || (e->state == PSYS_SVC_STOPPED && e->due_kind == DUE_RESTART)))
            settled = false;
    }
    svc_unlock();
    return settled;
}
