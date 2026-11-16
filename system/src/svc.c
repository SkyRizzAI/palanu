/* Service supervisor (system/include/pal/svc.h). One task (the supervisor) does all the work that changes a
 * service's state: it spawns installed services, drains their pipes into the logs, reaps them, moves them through their
 * states, runs the restart backoff and the memory retries, and evaluates the gates. Manual start and stop from the shell
 * are sent to it as requests, so a service started from a shell is a child of the supervisor and not of that shell (a
 * shell's session ends its children, see nema_proc_signal_tree).
 *
 * Waiting: the supervisor waits with nema_wait_on on the control object, the read end of each running service's pipe,
 * and each running service's exit object. The timeout is the next deadline (at most one second), so there is no
 * polling. When no supervisor task exists (host tests), psys_svc_step drives the same code with a fake clock. */
#include <stdio.h>
#include <string.h>

#include "pal/bootcount.h"
#include "pal/sysinfo.h"
#include "svc_int.h"

svc_global_t *svc_g; /* allocated by svc_ensure_init */

static int nop_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_NOSYS; }
static int nop_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_NOSYS; }
static uint32_t nop_poll(nema_obj_t *o, uint32_t e) { (void)o, (void)e; return 0; }
static nema_err_t nop_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t NOP_OPS = {"svc", nop_read, nop_write, nop_poll, nop_ioctl, NULL};

void svc_ensure_init(void) {
    if (svc_g) return; /* the first call is at boot, before any other task exists */
    nema_owner_t owner = nema_owner_register("svc");
    svc_g = (svc_global_t *)nema_calloc(NEMA_MEM_ANY, owner, sizeof(*svc_g));
    NEMA_CHECK(svc_g != NULL);
    svc_g->owner = owner;
    svc_g->lock = nema_port_mutex_create();
    svc_g->req_lock = nema_port_mutex_create();
    nema_obj_init(&svc_g->control, &NOP_OPS);
    nema_obj_init(&svc_g->reply, &NOP_OPS);
}

void svc_lock(void) { nema_port_mutex_lock(svc_g->lock); }
void svc_unlock(void) { nema_port_mutex_unlock(svc_g->lock); }
uint64_t svc_now_us(void) { return svc_g->clock ? svc_g->clock() : pal_sys_uptime_us(); }

void svc_setstr(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

bool svc_valid_id(const char *id) {
    if (!id[0]) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

void svc_set_state(svc_t *e, psys_svc_state_t s, const char *why) {
    e->state = s;
    svc_setstr(e->reason, sizeof(e->reason), why);
}

svc_t *svc_find(const char *id) {
    for (size_t i = 0; i < PSYS_SVC_MAX; i++)
        if (svc_g->t[i].used && strcmp(svc_g->t[i].id, id) == 0) return &svc_g->t[i];
    return NULL;
}

svc_t *svc_add(void) {
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        if (svc_g->t[i].used) continue;
        svc_t *e = &svc_g->t[i];
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->state = PSYS_SVC_STOPPED;
        nema_obj_init(&e->log_obj, &NOP_OPS);
        return e;
    }
    return NULL;
}

/* ---- the saved choices (/data/services) ---- */







/* ---- installed services: the registry is the source ---- */

svc_t *svc_ensure_installed(const char *id) {
    svc_t *e = svc_find(id);
    if (e) return e;
    pal_reg_app_t reg;
    if (!psys_reg_find(id, &reg) || reg.source != PAL_SRC_INSTALLED || reg.kind != PAL_KIND_SERVICE) return NULL;
    e = svc_add();
    if (!e) return NULL;
    e->installed = true;
    svc_setstr(e->id, sizeof(e->id), id);
    svc_setstr(e->name, sizeof(e->name), reg.name[0] ? reg.name : id);
    e->restart = PSYS_SVC_RESTART_ON_FAILURE;
    e->enabled = svc_saved_ovr(id) == OVR_ON;
    svc_set_state(e, PSYS_SVC_STOPPED, "not started");
    return e;
}

void svc_sync_installed(void) {
    size_t n = psys_reg_count();
    for (size_t i = 0; i < n; i++) {
        pal_reg_app_t reg;
        if (psys_reg_get(i, &reg) && reg.source == PAL_SRC_INSTALLED && reg.kind == PAL_KIND_SERVICE) svc_ensure_installed(reg.id);
    }
}

bool svc_visible(const svc_t *e) {
    if (!e->used) return false;
    if (!e->installed) return true;
    pal_reg_app_t r;
    return psys_reg_find(e->id, &r) && r.source == PAL_SRC_INSTALLED;
}

svc_t *svc_find_or_ensure(const char *id) {
    svc_t *e = svc_find(id);
    return e ? e : svc_ensure_installed(id);
}

/* ---- the log: a ring of PSYS_SVC_LOG_BYTES, and one line at a time to the nema log ---- */






/* ---- process state ---- */

bool svc_proc_gone(const svc_t *e) {
    return e->pid != 0 && (!e->proc || e->proc->pid != e->pid || e->proc->state != NEMA_PROC_RUNNING);
}

bool svc_pred_gone(void *ctx) { return svc_proc_gone((const svc_t *)ctx); }

/* Waits up to ms for the process to exit. Lock held. */
bool svc_wait_gone(svc_t *e, uint32_t ms) {
    if (!e->exit_obj) return true;
    nema_obj_t *o = e->exit_obj;
    return nema_wait_on(&o, 1, svc_pred_gone, e, (int32_t)ms) == NEMA_OK;
}

/* ---- starts, waits, failures ---- */

static void run_started(svc_t *e, uint64_t now) {
    e->started_us = now;
    svc_set_state(e, PSYS_SVC_RUNNING, "");
}

static void wait_memory(svc_t *e, uint64_t now, const char *why) {
    svc_set_state(e, PSYS_SVC_WAITING_MEMORY, why);
    e->due_us = now + (uint64_t)RETRY_MS * 1000u;
    e->due_kind = DUE_RETRY;
}

/* A failure: counted in the ten-minute window. The fifth within it is FAILED; otherwise a restart after the backoff
 * (1 s doubling to 60 s) when the policy allows one. */
static void note_failure(svc_t *e, uint64_t now, const char *what) {
    if (e->nfail == PSYS_SVC_FAIL_MAX) {
        memmove(e->fail_us, e->fail_us + 1, (PSYS_SVC_FAIL_MAX - 1) * sizeof(e->fail_us[0]));
        e->nfail--;
    }
    e->fail_us[e->nfail++] = now;
    uint32_t in = 0;
    for (uint8_t k = 0; k < e->nfail; k++)
        if (now - e->fail_us[k] < (uint64_t)PSYS_SVC_FAIL_WINDOW_MS * 1000u) in++;
    e->failures = in;
    char why[PSYS_SVC_REASON_LEN];
    if (e->restart == PSYS_SVC_RESTART_NEVER) {
        snprintf(why, sizeof(why), "%s; restart policy is never", what);
        svc_set_state(e, PSYS_SVC_FAILED, why);
        NEMA_LOGE(e->id, "%s", why);
        return;
    }
    if (in >= PSYS_SVC_FAIL_MAX) {
        snprintf(why, sizeof(why), "%s; failed %d times within ten minutes (start it by hand to try again)", what, (int)in);
        svc_set_state(e, PSYS_SVC_FAILED, why);
        NEMA_LOGE(e->id, "%s", why);
        return;
    }
    uint32_t delay = PSYS_SVC_BACKOFF_MS << (in - 1);
    if (delay > PSYS_SVC_BACKOFF_MAX_MS) delay = PSYS_SVC_BACKOFF_MAX_MS;
    snprintf(why, sizeof(why), "%s; restart in %lu s (failure %d of %d)", what, (unsigned long)(delay / 1000u), (int)in,
             PSYS_SVC_FAIL_MAX);
    svc_set_state(e, PSYS_SVC_STOPPED, why);
    e->due_us = now + (uint64_t)delay * 1000u;
    e->due_kind = DUE_RESTART;
    NEMA_LOGW(e->id, "%s", why);
}

static void start_builtin(svc_t *e, uint64_t now) {
    if (!e->bi.start) {
        run_started(e, now); /* started by boot (desktop, input) */
        return;
    }
    svc_set_state(e, PSYS_SVC_STARTING, "starting");
    nema_err_t r = e->bi.start();
    if (r == NEMA_OK) {
        run_started(e, now);
    } else if (r == NEMA_ERR_NOMEM) {
        wait_memory(e, now, "not enough memory to start; retrying every 5 s");
    } else {
        char what[64];
        snprintf(what, sizeof(what), "could not start (error %d)", (int)r);
        note_failure(e, now, what);
    }
}

static void start_installed(svc_t *e, uint64_t now) {
    pal_reg_app_t reg;
    if (!psys_reg_find(e->id, &reg) || reg.source != PAL_SRC_INSTALLED) {
        svc_set_state(e, PSYS_SVC_FAILED, "not installed");
        return;
    }
    char msg[120] = "";
    if (psys_reg_admit(&reg, -1, msg, sizeof(msg)) != NEMA_OK) {
        wait_memory(e, now, msg[0] ? msg : "refused by admission; retrying every 5 s");
        return;
    }
    nema_obj_t *rd = NULL, *wr = NULL;
    if (nema_pipe_create(PIPE_BYTES, NEMA_MEM_ANY, svc_g->owner, &rd, &wr) != NEMA_OK) {
        wait_memory(e, now, "no memory for its output pipe; retrying every 5 s");
        return;
    }
    svc_ensure_log(e);
    svc_set_state(e, PSYS_SVC_STARTING, "starting");
    nema_spawn_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.name = e->id;
    sp.stdio[0] = nema_null_obj(); /* a service reads nothing from the console */
    sp.stdio[1] = wr;
    sp.stdio[2] = wr;
    sp.cwd = "/";
    nema_pid_t pid = 0;
    e->ready_req = false; /* before the spawn: the new process may call pal_svc_ready at once */
    nema_err_t r = nema_proc_spawn(&sp, &pid);
    nema_obj_unref(wr); /* the process holds its own references now */
    if (r != NEMA_OK) {
        nema_obj_unref(rd);
        if (r == NEMA_ERR_NOMEM || r == NEMA_ERR_FULL) {
            wait_memory(e, now, "not enough memory to start; retrying every 5 s");
        } else {
            char what[64];
            snprintf(what, sizeof(what), "could not start (error %d)", (int)r);
            note_failure(e, now, what);
        }
        return;
    }
    e->pid = pid;
    e->proc = nema_proc_get(pid);
    e->exit_obj = nema_proc_exit_obj(pid);
    e->rd = rd;
    e->rd_eof = false;
    nema_obj_set_nonblock(rd, true);
    e->ready_due_us = now + (uint64_t)READY_TIMEOUT_MS * 1000u;
    e->started_us = now;
    char why[PSYS_SVC_REASON_LEN];
    snprintf(why, sizeof(why), "started, pid %d; waiting for pal_svc_ready", (int)pid);
    svc_set_state(e, PSYS_SVC_STARTING, why);
    NEMA_LOGI(e->id, "started (pid %d)", (int)pid);
}

static void start_entry(svc_t *e, uint64_t now) {
    e->due_us = 0;
    e->due_kind = DUE_NONE;
    e->ready_due_us = 0;
    e->stop_req = false;
    if (e->installed) start_installed(e, now);
    else start_builtin(e, now);
}

/* A process has gone: collect its status and decide what follows (the restart policy, the memory retry). */
static void reap(svc_t *e, uint64_t now) {
    int code = -1;
    if (nema_proc_wait(e->pid, &code, 0) != NEMA_OK) code = -1;
    svc_close_rd(e);
    e->pid = 0;
    e->proc = NULL;
    e->exit_obj = NULL;
    e->ready_due_us = 0;
    bool asked = e->stop_req;
    e->stop_req = false;
    if (asked) {
        svc_set_state(e, PSYS_SVC_STOPPED, "stopped");
        return;
    }
    if (code == EX_TEMPFAIL) {
        NEMA_LOGW(e->id, "refused at start (not enough memory); retrying every 5 s");
        wait_memory(e, now, "refused at start (not enough memory); retrying every 5 s");
        return;
    }
    if (code == 0) {
        if (e->restart == PSYS_SVC_RESTART_ALWAYS) {
            svc_set_state(e, PSYS_SVC_STOPPED, "exited (code 0); restart in 1 s");
            e->due_us = now + (uint64_t)PSYS_SVC_BACKOFF_MS * 1000u;
            e->due_kind = DUE_RESTART;
        } else {
            svc_set_state(e, PSYS_SVC_STOPPED, "exited (code 0)");
        }
        return;
    }
    char what[64];
    snprintf(what, sizeof(what), "exited with code %d", code);
    note_failure(e, now, what);
}

/* Stops a service. Manual stops are recorded by the caller; a gate's stop is not. Lock held. */
static void stop_entry(svc_t *e, const char *why, uint64_t now) {
    e->due_us = 0;
    e->due_kind = DUE_NONE;
    e->ready_due_us = 0;
    if (e->installed) {
        if (e->pid && !svc_proc_gone(e)) {
            e->stop_req = true;
            nema_proc_signal(e->pid, NEMA_SIG_TERM); /* the kernel kills it after its grace period */
            if (!svc_wait_gone(e, NEMA_TERM_GRACE_MS + 1000u)) {
                nema_proc_signal(e->pid, NEMA_SIG_KILL);
                svc_wait_gone(e, STOP_KILL_WAIT_MS);
            }
        }
        if (e->pid && svc_proc_gone(e)) {
            reap(e, now);
            svc_set_state(e, PSYS_SVC_STOPPED, why);
        } else if (e->pid) {
            svc_set_state(e, e->state, "stopping: the process has not exited yet");
        } else {
            e->stop_req = false;
            svc_set_state(e, PSYS_SVC_STOPPED, why);
        }
        return;
    }
    if (e->bi.stop && (e->state == PSYS_SVC_RUNNING || e->state == PSYS_SVC_STARTING)) e->bi.stop();
    svc_set_state(e, PSYS_SVC_STOPPED, why);
}

/* ---- requests from the shell (see the file comment) ---- */

static nema_err_t result_of(const svc_t *e) { return e->state == PSYS_SVC_FAILED ? NEMA_ERR_IO : NEMA_OK; }

static nema_err_t do_start(svc_t *e, uint64_t now) {
    svc_record_choice(e, true);
    if (e->state == PSYS_SVC_RUNNING || e->state == PSYS_SVC_STARTING) return NEMA_OK;
    if (e->state == PSYS_SVC_WAITING_MEMORY) svc_set_state(e, PSYS_SVC_STOPPED, "");
    e->nfail = 0;
    e->failures = 0;
    e->due_us = 0;
    if (e->state == PSYS_SVC_FAILED) svc_set_state(e, PSYS_SVC_STOPPED, "");
    if (e->bi.gate && !e->bi.gate()) { /* the choice is kept; the gate starts it when its condition holds */
        svc_set_state(e, PSYS_SVC_STOPPED, e->bi.waiting_for ? e->bi.waiting_for : "waiting for a condition");
        return NEMA_OK;
    }
    start_entry(e, now);
    return result_of(e);
}

static nema_err_t do_stop(svc_t *e, uint64_t now) {
    if (e->bi.core) return NEMA_ERR_PERM;
    svc_record_choice(e, false);
    stop_entry(e, "stopped by hand", now);
    return NEMA_OK;
}

static nema_err_t perform(int op, const char *id) {
    uint64_t now = svc_now_us();
    svc_t *e = svc_find_or_ensure(id);
    if (!e) return NEMA_ERR_NOENT;
    if (op == REQ_START) return do_start(e, now);
    if (op == REQ_STOP) return do_stop(e, now);
    nema_err_t r = do_stop(e, now);
    if (r != NEMA_OK) return r;
    return do_start(e, now);
}

/* Lock held: the supervisor's whole job for one pass at time now. Returns the next deadline (0 if none). */
static uint64_t step_locked(uint64_t now) {
    svc_g->kick = false;
    if (svc_g->req_op != REQ_NONE) {
        svc_g->req_result = perform(svc_g->req_op, svc_g->req_id);
        svc_g->req_op = REQ_NONE;
        svc_g->req_done = true;
        nema_notify(&svc_g->reply);
    }
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        svc_t *e = &svc_g->t[i];
        if (!e->used || !e->installed) continue;
        svc_drain(e);
        if (e->pid && svc_proc_gone(e)) reap(e, now);
    }
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        svc_t *e = &svc_g->t[i];
        if (!e->used || e->state != PSYS_SVC_STARTING || !e->pid || !e->ready_due_us) continue;
        if (e->ready_req || now >= e->ready_due_us) {
            e->ready_due_us = 0;
            svc_set_state(e, PSYS_SVC_RUNNING, e->ready_req ? "" : "running (no pal_svc_ready call)");
        }
    }
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        svc_t *e = &svc_g->t[i];
        if (!e->used || e->due_us == 0 || now < e->due_us) continue;
        uint8_t kind = e->due_kind;
        e->due_us = 0;
        e->due_kind = DUE_NONE;
        if (e->state != PSYS_SVC_STOPPED && e->state != PSYS_SVC_WAITING_MEMORY) continue;
        if (!e->enabled || (svc_g->safe && !e->bi.core)) continue; /* safe mode: nothing restarts but core services */
        if (e->bi.gate && !e->bi.gate()) {
            svc_set_state(e, PSYS_SVC_STOPPED, e->bi.waiting_for ? e->bi.waiting_for : "waiting for a condition");
            continue;
        }
        if (kind == DUE_RESTART) e->restarts++;
        start_entry(e, now);
    }
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        svc_t *e = &svc_g->t[i];
        if (!e->used || !e->bi.gate || e->installed) continue;
        bool open = e->bi.gate();
        bool may_run = svc_g->safe ? e->bi.core : true;
        if (open && e->enabled && may_run && e->state == PSYS_SVC_STOPPED && e->due_us == 0 && !e->stop_req) {
            start_entry(e, now);
        } else if (!open && (e->state == PSYS_SVC_RUNNING || e->state == PSYS_SVC_STARTING || e->state == PSYS_SVC_WAITING_MEMORY)) {
            stop_entry(e, e->bi.waiting_for ? e->bi.waiting_for : "waiting for a condition", now);
        }
    }
    if (svc_g->started) {
        bool settled = true;
        for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
            svc_t *e = &svc_g->t[i];
            if (e->used && (e->state == PSYS_SVC_STARTING || (e->state == PSYS_SVC_STOPPED && e->due_kind == DUE_RESTART)))
                settled = false;
        }
        psys_bootcount_ready_check(now, settled);
    }
    uint64_t next = 0;
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        const svc_t *e = &svc_g->t[i];
        if (!e->used) continue;
        if (e->due_us && (next == 0 || e->due_us < next)) next = e->due_us;
        if (e->ready_due_us && (next == 0 || e->ready_due_us < next)) next = e->ready_due_us;
    }
    return next;
}

/* ---- the supervisor task and its wait set ---- */

static bool pred_work(void *ctx) {
    (void)ctx;
    if (svc_g->kick || svc_g->req_op != REQ_NONE) return true;
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        const svc_t *e = &svc_g->t[i];
        if (!e->used) continue;
        if (e->rd && !e->rd_eof && nema_obj_poll(e->rd, NEMA_POLL_IN)) return true;
        if (e->pid && svc_proc_gone(e)) return true;
    }
    return false;
}

static bool pred_idle(void *ctx) {
    (void)ctx;
    return svc_g->req_op == REQ_NONE;
}

static bool pred_done(void *ctx) {
    (void)ctx;
    return svc_g->req_done;
}

/* Lock held. Fills objs with what the supervisor waits on; returns how many. */
static size_t waitset_locked(nema_obj_t **objs) {
    size_t n = 0;
    objs[n++] = &svc_g->control;
    for (size_t i = 0; i < PSYS_SVC_MAX && n < WAITSET_MAX; i++) {
        const svc_t *e = &svc_g->t[i];
        if (!e->used) continue;
        if (e->rd && !e->rd_eof && n < WAITSET_MAX) objs[n++] = e->rd;
        if (e->pid && e->exit_obj && n < WAITSET_MAX) objs[n++] = e->exit_obj;
    }
    return n;
}

static size_t s_stack_spare = (size_t)-1; /* smallest spare stack the supervisor has ever had, bytes */
size_t psys_svc_stack_spare(void) { return s_stack_spare; }

static void supervisor(void *arg) {
    (void)arg;
    for (;;) {
        size_t spare = nema_port_task_stack_free();
        if (spare < s_stack_spare) s_stack_spare = spare;
        nema_obj_t *objs[WAITSET_MAX];
        svc_lock();
        uint64_t now = svc_now_us();
        uint64_t next = step_locked(now);
        size_t n = waitset_locked(objs);
        svc_unlock();
        uint32_t span = SUPERVISOR_TICK_MS;
        if (next > now && (next - now) / 1000u < span) span = (uint32_t)((next - now) / 1000u);
        nema_wait_on(objs, n, pred_work, NULL, (int32_t)span);
    }
}

/* ---- public API ---- */

nema_err_t psys_svc_register_builtin(const psys_svc_builtin_t *b) {
    svc_ensure_init();
    if (!b || !b->id || strlen(b->id) >= PSYS_SVC_ID_LEN || !svc_valid_id(b->id)) return NEMA_ERR_INVAL;
    svc_lock();
    if (svc_find(b->id)) {
        svc_unlock();
        return NEMA_ERR_EXIST;
    }
    svc_t *e = svc_add();
    if (!e) {
        svc_unlock();
        return NEMA_ERR_FULL;
    }
    e->bi = *b;
    svc_setstr(e->id, sizeof(e->id), b->id);
    svc_setstr(e->name, sizeof(e->name), b->name ? b->name : b->id);
    e->restart = b->restart;
    e->enabled = b->autostart;
    svc_unlock();
    return NEMA_OK;
}

/* Lock held. Orders the table: a service comes after the ids in its `after` list that are present. */
static void boot_order(int *order, size_t *count) {
    bool done[PSYS_SVC_MAX] = {false};
    size_t cnt = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
            const svc_t *e = &svc_g->t[i];
            if (!e->used || done[i]) continue;
            bool ready = true;
            if (!e->installed && e->bi.after) {
                for (const char *const *a = e->bi.after; *a && ready; a++) {
                    const svc_t *d = svc_find(*a);
                    if (!d || d == e) continue;
                    size_t j = (size_t)(d - svc_g->t);
                    if (!done[j]) ready = false;
                }
            }
            if (!ready) continue;
            done[i] = true;
            order[cnt++] = (int)i;
            progress = true;
        }
    }
    *count = cnt;
}

void psys_svc_start_all(bool safe_mode) {
    svc_ensure_init();
    svc_lock();
    svc_g->safe = safe_mode;
    svc_store_load();
    svc_sync_installed();
    for (size_t i = 0; i < PSYS_SVC_MAX; i++)
        if (svc_g->t[i].used) svc_apply_saved(&svc_g->t[i]);
    int order[PSYS_SVC_MAX];
    size_t n = 0;
    boot_order(order, &n);
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        svc_t *e = &svc_g->t[i];
        if (!e->used) continue;
        bool in_order = false;
        for (size_t k = 0; k < n; k++) in_order = in_order || order[k] == (int)i;
        if (!in_order) {
            svc_set_state(e, PSYS_SVC_FAILED, "after: dependency cycle; not started");
            NEMA_LOGE("svc", "%s: dependency cycle in after", e->id);
        }
    }
    uint64_t now = svc_now_us();
    for (size_t k = 0; k < n; k++) {
        svc_t *e = &svc_g->t[order[k]];
        if (e->bi.core) {
            start_entry(e, now);
            continue;
        }
        if (svc_g->safe) {
            svc_set_state(e, PSYS_SVC_STOPPED, "not started: safe mode");
        } else if (!e->enabled) {
            svc_set_state(e, PSYS_SVC_STOPPED, "disabled");
        } else if (e->bi.gate && !e->bi.gate()) {
            svc_set_state(e, PSYS_SVC_STOPPED, e->bi.waiting_for ? e->bi.waiting_for : "waiting for a condition");
        } else {
            start_entry(e, now);
        }
    }
    svc_g->started = true;
    if (!svc_g->no_task && !svc_g->task_on) {
        nema_task_desc_t td = {.name = "svc", .fn = supervisor, .arg = NULL, .stack = SUPERVISOR_STACK, .prio = 4,
                               .core = -1, .role = NEMA_TASK_SYSTEM, .owner = svc_g->owner};
        if (nema_task_create(&td) == NEMA_OK) svc_g->task_on = true;
        else NEMA_LOGE("svc", "supervisor task could not be created");
    }
    svc_unlock();
    NEMA_LOGI("svc", "%lu services known, %s", (unsigned long)n, safe_mode ? "safe mode: core services only" : "started");
}

bool psys_svc_safe_mode(void) { return svc_g && svc_g->safe; }

void psys_svc_poke(void) {
    if (!svc_g) return; /* before boot there is nothing to wake */
    svc_g->kick = true;
    nema_notify(&svc_g->control);
}

/* A request: the supervisor does it when it runs; with no supervisor task (host tests, before boot) the caller does it. */
static nema_err_t submit(int op, const char *id) {
    svc_ensure_init();
    if (!id || !id[0] || strlen(id) >= PSYS_SVC_ID_LEN) return NEMA_ERR_INVAL;
    nema_port_mutex_lock(svc_g->req_lock);
    svc_lock();
    if (!svc_g->task_on) {
        nema_err_t r = perform(op, id);
        svc_unlock();
        nema_port_mutex_unlock(svc_g->req_lock);
        return r;
    }
    svc_unlock();
    nema_obj_t *o = &svc_g->reply;
    if (nema_wait_on(&o, 1, pred_idle, NULL, REQ_TIMEOUT_MS) != NEMA_OK) {
        nema_port_mutex_unlock(svc_g->req_lock);
        return NEMA_ERR_TIMEOUT; /* an earlier request is still running */
    }
    svc_lock();
    svc_g->req_op = op;
    svc_setstr(svc_g->req_id, sizeof(svc_g->req_id), id);
    svc_g->req_done = false;
    svc_g->req_result = NEMA_OK;
    svc_unlock();
    psys_svc_poke();
    int w = nema_wait_on(&o, 1, pred_done, NULL, REQ_TIMEOUT_MS);
    svc_lock();
    nema_err_t r = w == NEMA_OK ? svc_g->req_result : (w == NEMA_ERR_INTR ? NEMA_ERR_INTR : NEMA_ERR_TIMEOUT);
    svc_unlock();
    nema_port_mutex_unlock(svc_g->req_lock);
    return r;
}

nema_err_t psys_svc_start(const char *id) { return submit(REQ_START, id); }
nema_err_t psys_svc_stop(const char *id) { return submit(REQ_STOP, id); }
nema_err_t psys_svc_restart(const char *id) { return submit(REQ_RESTART, id); }

nema_err_t psys_svc_set_enabled(const char *id, bool on) {
    svc_ensure_init();
    svc_lock();
    svc_t *e = svc_find_or_ensure(id);
    if (e) svc_record_choice(e, on);
    svc_unlock();
    return e ? NEMA_OK : NEMA_ERR_NOENT;
}

nema_err_t psys_svc_ready_self(void) {
    nema_proc_t *self = nema_self();
    if (!svc_g || !self || !self->app) return NEMA_ERR_NOENT;
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        svc_t *e = &svc_g->t[i];
        if (!e->used || !e->installed || e->state != PSYS_SVC_STARTING) continue;
        /* the pid is set once spawn returns; a process that asks earlier is matched by its name */
        if (e->pid == self->pid || (e->pid == 0 && strcmp(e->id, self->app->name) == 0)) {
            e->ready_req = true;
            psys_svc_poke();
            return NEMA_OK;
        }
    }
    return NEMA_ERR_NOENT;
}












void psys_svc_set_clock(uint64_t (*now)(void)) {
    svc_ensure_init();
    svc_g->clock = now;
}

void psys_svc_test_no_task(void) {
    svc_ensure_init();
    svc_g->no_task = true;
}

uint64_t psys_svc_step(uint64_t now_us_value) {
    svc_ensure_init();
    svc_lock();
    uint64_t next = step_locked(now_us_value);
    svc_unlock();
    return next;
}

void psys_svc_test_reset(void) {
    svc_ensure_init();
    svc_lock();
    for (size_t i = 0; i < PSYS_SVC_MAX; i++) {
        svc_t *e = &svc_g->t[i];
        if (!e->used) continue;
        if (e->installed) svc_close_rd(e);
        if (e->log) nema_free(e->log);
    }
    memset(svc_g->t, 0, sizeof(svc_g->t));
    memset(svc_g->saved, 0, sizeof(svc_g->saved));
    svc_g->store_loaded = false;
    svc_g->safe = false;
    svc_g->started = false;
    svc_g->kick = false;
    svc_g->req_op = REQ_NONE;
    svc_unlock();
}
