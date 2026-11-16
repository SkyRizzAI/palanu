#include <stdio.h>
#include <setjmp.h>
#include <string.h>

#include "internal.h"

#define PID_MAX 30000
#define APPS_INITIAL 32u /* the registry starts small and doubles in PSRAM: no fixed limit for built-ins to eat */

static struct {
    nema_port_mutex_t *lock;
    nema_proc_t proc[NEMA_PROC_MAX];
    nema_pid_t next_pid;
    const nema_app_t **apps; /* kernel-owned table, grown by doubling; guarded by lock */
    size_t napps, capps;
    bool reaper_on; /* a reaper task is alive (guarded by lock) */
} P;

static int sig_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static int sig_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static uint32_t sig_poll(nema_obj_t *o, uint32_t e) { (void)o, (void)e; return 0; }
static nema_err_t sig_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t EVENT_OPS = {"event", sig_read, sig_write, sig_poll, sig_ioctl, NULL};

void nema_proc_init(void) {
    memset(&P, 0, sizeof(P));
    P.lock = nema_port_mutex_create();
    P.next_pid = 1;
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        nema_obj_init(&P.proc[i].sig_obj, &EVENT_OPS);
        nema_obj_init(&P.proc[i].exit_obj, &EVENT_OPS);
        P.proc[i].owner = (nema_owner_t)(NEMA_OWNER_PROC_BASE + i);
    }
}

/* ---- app registry (caller holds P.lock for the *_locked helpers) ---- */
static const nema_app_t *find_app_locked(const char *name) {
    for (size_t i = 0; i < P.napps; i++)
        if (strcmp(P.apps[i]->name, name) == 0) return P.apps[i];
    return NULL;
}

nema_err_t nema_app_register(const nema_app_t *app) {
    if (!app || !app->name || !app->main) return NEMA_ERR_INVAL;
    for (;;) {
        nema_port_mutex_lock(P.lock);
        if (find_app_locked(app->name)) {
            nema_port_mutex_unlock(P.lock);
            return NEMA_ERR_EXIST;
        }
        if (P.napps < P.capps) {
            P.apps[P.napps++] = app;
            nema_port_mutex_unlock(P.lock);
            return NEMA_OK;
        }
        size_t want = P.capps ? P.capps * 2 : APPS_INITIAL;
        nema_port_mutex_unlock(P.lock);
        /* Allocate without the lock (the allocator may run reclaimers), then swap in under it. */
        const nema_app_t **grown = (const nema_app_t **)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, want * sizeof(*grown));
        if (!grown) return NEMA_ERR_NOMEM;
        nema_port_mutex_lock(P.lock);
        if (P.capps >= want) { /* another registration grew the table first */
            nema_port_mutex_unlock(P.lock);
            nema_free(grown);
            continue;
        }
        if (P.napps) memcpy(grown, P.apps, P.napps * sizeof(*grown));
        const nema_app_t **old = P.apps;
        P.apps = grown;
        P.capps = want;
        nema_port_mutex_unlock(P.lock);
        nema_free(old);
    }
}

/* Removes a command. Refused with NEMA_ERR_BUSY while a process runs it: the registration may own memory that the
 * running process still reads (a removed .papp), and the caller frees that memory only after this returns OK. */
nema_err_t nema_app_unregister(const char *name) {
    nema_err_t r = NEMA_ERR_NOENT;
    nema_port_mutex_lock(P.lock);
    for (size_t i = 0; i < P.napps; i++) {
        if (strcmp(P.apps[i]->name, name) != 0) continue;
        r = NEMA_OK;
        for (int s = 0; s < NEMA_PROC_MAX; s++)
            if (P.proc[s].state == NEMA_PROC_RUNNING && P.proc[s].app == P.apps[i]) r = NEMA_ERR_BUSY;
        if (r == NEMA_OK) {
            memmove(&P.apps[i], &P.apps[i + 1], (P.napps - i - 1) * sizeof(*P.apps));
            P.napps--;
        }
        break;
    }
    nema_port_mutex_unlock(P.lock);
    return r;
}

const nema_app_t *nema_app_find(const char *name) {
    nema_port_mutex_lock(P.lock);
    const nema_app_t *found = find_app_locked(name);
    nema_port_mutex_unlock(P.lock);
    return found;
}

size_t nema_app_count(void) {
    nema_port_mutex_lock(P.lock);
    size_t n = P.napps;
    nema_port_mutex_unlock(P.lock);
    return n;
}

const nema_app_t *nema_app_at(size_t i) {
    nema_port_mutex_lock(P.lock);
    const nema_app_t *a = i < P.napps ? P.apps[i] : NULL;
    nema_port_mutex_unlock(P.lock);
    return a;
}

/* ---- self and signals ---- */
nema_proc_t *nema_self(void) {
    nema_tctx_t *c = (nema_tctx_t *)nema_port_tls_get();
    return c ? c->proc : NULL;
}

bool nema_interrupted(void) {
    nema_proc_t *p = nema_self();
    return p && p->defer == 0 && (p->sigpending & NEMA_SIG_FATAL) != 0;
}

bool nema_canceled(void) {
    nema_proc_t *p = nema_self();
    return p && p->defer == 0 && p->killed;
}

static void proc_finish(nema_proc_t *p, int code);

void nema_cancel_strike(void) {
    nema_proc_t *p = nema_self();
    if (!p || !p->killed || p->defer) return;
    if (++p->cancel_strikes < NEMA_CANCEL_STRIKES) return;
    /* The process went on calling blocking functions after it was killed, so it will never return by itself. We are on
     * its own task at a call boundary and no kernel lock is held, so jump back to proc_entry: it runs proc_finish and
     * returns to the task trampoline, which does the task accounting. The app's own frames are abandoned. */
    if (p->exit_jmp) longjmp(*(jmp_buf *)p->exit_jmp, 1);
}

void nema_cancel_defer(void) {
    nema_proc_t *p = nema_self();
    if (p) p->defer++;
}

void nema_cancel_allow(void) {
    nema_proc_t *p = nema_self();
    if (p && p->defer) p->defer--;
}

uint32_t nema_sig_take(uint32_t mask) {
    nema_proc_t *p = nema_self();
    if (!p) return 0;
    nema_port_mutex_lock(P.lock);
    uint32_t taken = p->sigpending & mask;
    p->sigpending &= ~mask;
    nema_port_mutex_unlock(P.lock);
    return taken;
}

static nema_proc_t *find_pid_locked(nema_pid_t pid) {
    for (int i = 0; i < NEMA_PROC_MAX; i++)
        if (P.proc[i].state != NEMA_PROC_FREE && P.proc[i].pid == pid) return &P.proc[i];
    return NULL;
}

/* The exit notification of a live process: notified when it becomes a zombie. Valid until it is reaped. */
nema_obj_t *nema_proc_exit_obj(nema_pid_t pid) {
    nema_port_mutex_lock(P.lock);
    nema_proc_t *p = find_pid_locked(pid);
    nema_obj_t *o = p ? &p->exit_obj : NULL;
    nema_port_mutex_unlock(P.lock);
    return o;
}

nema_proc_t *nema_proc_get(nema_pid_t pid) {
    nema_port_mutex_lock(P.lock);
    nema_proc_t *p = find_pid_locked(pid);
    nema_port_mutex_unlock(P.lock);
    return p;
}

/* ---- cancellation (docs/adr/0003) ----
 * KILL poisons the process at once (caller holds P.lock). TERM starts a grace period that the reaper task enforces.
 * The reaper exists only while some process has a TERM pending, so an idle system has no extra task. */
static bool pred_never(void *ctx) {
    (void)ctx;
    return false;
}

static void poison_locked(nema_proc_t *p) {
    if (p->killed) return;
    p->killed = true;
    p->sigpending |= NEMA_SIG_KILL;
    if (p->kill_hook) p->kill_hook(p->kill_ctx);
}

/* Caller holds P.lock. Returns true when a TERM grace period now needs the reaper. */
static bool deliver_locked(nema_proc_t *p, uint32_t sig) {
    p->sigpending |= sig;
    if (sig & NEMA_SIG_KILL) poison_locked(p);
    if ((sig & NEMA_SIG_TERM) && !p->term_pending && !p->killed) {
        p->term_pending = true;
        p->term_at_us = nema_port_time_us() + (uint64_t)NEMA_TERM_GRACE_MS * 1000u;
        return true;
    }
    return false;
}

static void reaper_entry(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t now = nema_port_time_us(), next = 0;
        nema_proc_t *due[NEMA_PROC_MAX];
        int nd = 0;
        nema_port_mutex_lock(P.lock);
        for (int i = 0; i < NEMA_PROC_MAX; i++) {
            nema_proc_t *p = &P.proc[i];
            if (p->state != NEMA_PROC_RUNNING || !p->term_pending || p->killed) continue;
            if (p->term_at_us <= now) {
                poison_locked(p);
                due[nd++] = p;
            } else if (!next || p->term_at_us < next) {
                next = p->term_at_us;
            }
        }
        if (!nd && !next) {
            P.reaper_on = false;
            nema_port_mutex_unlock(P.lock);
            return;
        }
        nema_port_mutex_unlock(P.lock);
        for (int i = 0; i < nd; i++) nema_notify(&due[i]->sig_obj);
        if (next) {
            int32_t ms = (int32_t)((next - now + 999u) / 1000u);
            nema_wait_on(NULL, 0, pred_never, NULL, ms); /* sleeps until the earliest deadline */
        }
    }
}

/* Makes sure a reaper runs. False when there is no memory for it: the caller then kills at once. */
static bool reaper_start(void) {
    nema_port_mutex_lock(P.lock);
    if (P.reaper_on) {
        nema_port_mutex_unlock(P.lock);
        return true;
    }
    P.reaper_on = true;
    nema_port_mutex_unlock(P.lock);
    nema_task_desc_t td = {
        .name = "reaper",
        .fn = reaper_entry,
        .arg = NULL,
        .stack = 2048,
        .prio = 4,
        .core = -1,
        .role = NEMA_TASK_SERVICE,
        .owner = NEMA_OWNER_KERNEL,
    };
    if (nema_task_create(&td) == NEMA_OK) return true;
    nema_port_mutex_lock(P.lock);
    P.reaper_on = false;
    nema_port_mutex_unlock(P.lock);
    return false;
}

static void after_term(nema_proc_t *p) {
    if (reaper_start()) return;
    nema_port_mutex_lock(P.lock);
    poison_locked(p);
    nema_port_mutex_unlock(P.lock);
    nema_notify(&p->sig_obj);
}

nema_err_t nema_proc_signal(nema_pid_t pid, uint32_t sig) {
    nema_port_mutex_lock(P.lock);
    nema_proc_t *p = find_pid_locked(pid);
    bool term = p && p->state == NEMA_PROC_RUNNING && deliver_locked(p, sig);
    bool running = p && p->state == NEMA_PROC_RUNNING;
    nema_port_mutex_unlock(P.lock);
    if (!running) return NEMA_ERR_NOENT;
    nema_notify(&p->sig_obj);
    if (term) after_term(p);
    return NEMA_OK;
}

int nema_proc_signal_group(nema_pid_t pgid, uint32_t sig) {
    nema_proc_t *hit[NEMA_PROC_MAX];
    int n = 0;
    bool term = false;
    nema_port_mutex_lock(P.lock);
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        if (P.proc[i].state == NEMA_PROC_RUNNING && P.proc[i].pgid == pgid) {
            term = deliver_locked(&P.proc[i], sig) || term;
            hit[n++] = &P.proc[i];
        }
    }
    nema_port_mutex_unlock(P.lock);
    for (int i = 0; i < n; i++) nema_notify(&hit[i]->sig_obj);
    if (term && !reaper_start()) {
        nema_port_mutex_lock(P.lock);
        for (int i = 0; i < n; i++) poison_locked(hit[i]);
        nema_port_mutex_unlock(P.lock);
        for (int i = 0; i < n; i++) nema_notify(&hit[i]->sig_obj);
    }
    return n;
}

int nema_proc_signal_tree(nema_pid_t root, uint32_t sig) {
    nema_proc_t *hit[NEMA_PROC_MAX];
    int n = 0;
    bool term = false;
    nema_port_mutex_lock(P.lock);
    for (int i = 0; i < NEMA_PROC_MAX; i++)
        if (P.proc[i].state == NEMA_PROC_RUNNING && P.proc[i].pid == root) hit[n++] = &P.proc[i];
    /* repeat until no new descendant turns up: a child can only appear after its parent in the walk, never before */
    for (bool grew = n > 0; grew;) {
        grew = false;
        for (int i = 0; i < NEMA_PROC_MAX; i++) {
            nema_proc_t *c = &P.proc[i];
            if (c->state != NEMA_PROC_RUNNING) continue;
            bool known = false, parent_in = false;
            for (int k = 0; k < n; k++) {
                if (hit[k] == c) known = true;
                if (hit[k]->pid == c->parent) parent_in = true;
            }
            if (!known && parent_in) {
                hit[n++] = c;
                grew = true;
            }
        }
    }
    for (int i = 0; i < n; i++) term = deliver_locked(hit[i], sig) || term;
    nema_port_mutex_unlock(P.lock);
    for (int i = 0; i < n; i++) nema_notify(&hit[i]->sig_obj);
    if (term && !reaper_start()) {
        nema_port_mutex_lock(P.lock);
        for (int i = 0; i < n; i++) poison_locked(hit[i]);
        nema_port_mutex_unlock(P.lock);
        for (int i = 0; i < n; i++) nema_notify(&hit[i]->sig_obj);
    }
    return n;
}

nema_err_t nema_proc_set_kill_hook(void (*fn)(void *ctx), void *ctx) {
    nema_proc_t *p = nema_self();
    if (!p) return NEMA_ERR_INVAL;
    nema_port_mutex_lock(P.lock);
    p->kill_hook = fn;
    p->kill_ctx = ctx;
    if (fn && p->killed) fn(ctx);
    nema_port_mutex_unlock(P.lock);
    return NEMA_OK;
}

/* ---- spawn and exit ---- */
static char **build_argv(nema_owner_t owner, const char *name, int argc, const char *const *argv, int *out_argc) {
    int n = argc > 0 ? argc : 1;
    size_t bytes = (size_t)(n + 1) * sizeof(char *);
    for (int i = 0; i < n; i++) bytes += strlen(argc > 0 ? argv[i] : name) + 1;
    char **v = (char **)nema_alloc_ex(NEMA_MEM_ANY, owner, bytes, 0, NEMA_ALLOC_ZERO);
    if (!v) return NULL;
    char *s = (char *)(v + n + 1);
    for (int i = 0; i < n; i++) {
        const char *src = argc > 0 ? argv[i] : name;
        v[i] = s;
        size_t l = strlen(src) + 1;
        memcpy(s, src, l);
        s += l;
    }
    v[n] = NULL;
    *out_argc = n;
    return v;
}

static void proc_finish(nema_proc_t *p, int code) {
    for (int i = 0; i < NEMA_HANDLES_MAX; i++) {
        nema_obj_t *o = p->fd[i];
        p->fd[i] = NULL;
        nema_obj_unref(o);
    }
    nema_free(p->argv);
    p->argv = NULL;
    /* Whatever the process still holds is freed now, before the slot can be reused, so nothing outlives it or gets
     * charged to the next process in the slot. Shared objects (pipes, TTYs) are never process-owned, see pipe.c. */
    size_t blocks = 0;
    size_t swept = nema_mem_owner_sweep(p->owner, &blocks);
    if (swept) NEMA_LOGW("proc", "%s[%d] exited holding %lu bytes (%lu blocks): swept", p->name, p->pid, (unsigned long)swept, (unsigned long)blocks);
    nema_port_mutex_lock(P.lock);
    /* read under the lock: nema_proc_detach may run while the process is finishing */
    bool detached = (p->flags & NEMA_SPAWN_DETACH) != 0;
    if (p->killed) code = 128 + 9; /* reported as killed, whatever the app returned */
    p->exit_code = code;
    p->term_pending = false;
    p->kill_hook = NULL;
    p->kill_ctx = NULL;
    p->state = detached ? NEMA_PROC_FREE : NEMA_PROC_ZOMBIE;
    nema_port_mutex_unlock(P.lock);
    nema_notify(&p->exit_obj);
}

nema_err_t nema_proc_detach(nema_pid_t pid) {
    nema_port_mutex_lock(P.lock);
    nema_proc_t *p = find_pid_locked(pid);
    if (!p) {
        nema_port_mutex_unlock(P.lock);
        return NEMA_ERR_NOENT;
    }
    p->flags |= NEMA_SPAWN_DETACH;
    if (p->state == NEMA_PROC_ZOMBIE) p->state = NEMA_PROC_FREE; /* already finished: nobody is left to reap it */
    nema_port_mutex_unlock(P.lock);
    return NEMA_OK;
}

static void proc_entry(void *arg) {
    nema_proc_t *p = (nema_proc_t *)arg;
    nema_tctx_t *tc = nema_tctx();
    tc->proc = p;
    jmp_buf jb;
    int code = 137;
    p->exit_jmp = &jb;
    if (setjmp(jb) == 0) code = p->app->main(p->argc, p->argv);
    p->exit_jmp = NULL;
    tc->proc = NULL; /* from here on, signals no longer interrupt the cleanup */
    proc_finish(p, code);
}

nema_err_t nema_proc_spawn(const nema_spawn_t *spec, nema_pid_t *pid_out) {
    if (!spec || !spec->name) return NEMA_ERR_INVAL;
    nema_proc_t *parent = nema_self();

    /* Look the app up and reserve the slot under one lock: a reserved slot that names the app makes unregister refuse,
     * so the app cannot be removed between the lookup and the start. */
    nema_proc_t *p = NULL;
    const nema_app_t *app = NULL;
    nema_port_mutex_lock(P.lock);
    app = find_app_locked(spec->name);
    for (int i = 0; app && i < NEMA_PROC_MAX && !p; i++) {
        if (P.proc[i].state == NEMA_PROC_FREE) {
            p = &P.proc[i];
            p->state = NEMA_PROC_RUNNING; /* reserve */
            p->pid = P.next_pid;
            P.next_pid = P.next_pid >= PID_MAX ? 1 : (nema_pid_t)(P.next_pid + 1);
            p->app = app;
            p->sigpending = 0;
            p->killed = false;
            p->cancel_strikes = 0;
            p->exit_jmp = NULL;
            p->term_pending = false;
            p->defer = 0;
            p->kill_hook = NULL;
            p->kill_ctx = NULL;
        }
    }
    nema_port_mutex_unlock(P.lock);
    if (!app) return NEMA_ERR_NOENT;
    if (!p) return NEMA_ERR_FULL;

    p->exit_code = 0;
    p->flags = spec->flags;
    p->parent = parent ? parent->pid : 0;
    p->pgid = spec->pgid > 0 ? spec->pgid : p->pid;
    p->start_us = nema_port_time_us();
    nema_strlcpy(p->name, app->name, sizeof(p->name));
    p->name[sizeof(p->name) - 1] = 0;
    nema_owner_set_name(p->owner, p->name);
    /* A new run starts with a clean slate: the quota of this run, zero peak and refusals. The slot has no blocks
     * left, because the previous run's exit swept them. */
    nema_mem_owner_begin(p->owner, spec->mem_quota_ext ? spec->mem_quota_ext : NEMA_PROC_QUOTA_EXT_DEFAULT,
                         spec->mem_quota_int ? spec->mem_quota_int : NEMA_PROC_QUOTA_INT_DEFAULT);
    const char *cwd = spec->cwd ? spec->cwd : (parent ? parent->cwd : "/");
    nema_strlcpy(p->cwd, cwd, sizeof(p->cwd));
    p->cwd[sizeof(p->cwd) - 1] = 0;

    p->argv = build_argv(p->owner, app->name, spec->argc, spec->argv, &p->argc);
    if (!p->argv) goto fail_nomem;
    for (int i = 0; i < NEMA_HANDLES_MAX; i++) p->fd[i] = NULL;
    for (int i = 0; i < 3; i++) {
        nema_obj_t *o = spec->stdio[i] ? spec->stdio[i] : (parent ? parent->fd[i] : NULL);
        p->fd[i] = nema_obj_ref(o ? o : nema_null_obj());
    }

    nema_task_desc_t td = {
        .name = app->name,
        .fn = proc_entry,
        .arg = p,
        .stack = app->stack ? app->stack : NEMA_APP_STACK_DEFAULT,
        .prio = 5,
        .core = -1,
        .role = NEMA_TASK_PROCESS,
        .owner = p->owner,
    };
    nema_err_t r = nema_task_create(&td);
    if (r != NEMA_OK) {
        for (int i = 0; i < NEMA_HANDLES_MAX; i++) {
            nema_obj_unref(p->fd[i]);
            p->fd[i] = NULL;
        }
        nema_free(p->argv);
        p->argv = NULL;
        nema_mem_owner_sweep(p->owner, NULL);
        nema_port_mutex_lock(P.lock);
        p->state = NEMA_PROC_FREE;
        nema_port_mutex_unlock(P.lock);
        return r;
    }
    if (pid_out) *pid_out = p->pid;
    return NEMA_OK;

fail_nomem:
    nema_port_mutex_lock(P.lock);
    p->state = NEMA_PROC_FREE;
    nema_port_mutex_unlock(P.lock);
    return NEMA_ERR_NOMEM;
}

typedef struct {
    nema_proc_t *p;
    nema_pid_t pid;
} wait_ctx_t;

static bool pred_zombie(void *c) {
    wait_ctx_t *w = (wait_ctx_t *)c;
    return w->p->pid != w->pid || w->p->state != NEMA_PROC_RUNNING;
}

nema_err_t nema_proc_wait(nema_pid_t pid, int *code, int32_t timeout_ms) {
    nema_port_mutex_lock(P.lock);
    nema_proc_t *p = find_pid_locked(pid);
    nema_port_mutex_unlock(P.lock);
    if (!p) return NEMA_ERR_NOENT;
    wait_ctx_t w = {p, pid};
    nema_obj_t *o = &p->exit_obj;
    int r = nema_wait_on(&o, 1, pred_zombie, &w, timeout_ms);
    if (r != NEMA_OK) return r;
    nema_err_t out = NEMA_ERR_NOENT;
    nema_port_mutex_lock(P.lock);
    if (p->pid == pid && p->state == NEMA_PROC_ZOMBIE) {
        if (code) *code = p->exit_code;
        p->state = NEMA_PROC_FREE;
        out = NEMA_OK;
    } else if (p->pid == pid && p->state == NEMA_PROC_FREE) {
        out = NEMA_ERR_NOENT; /* reaped by someone else */
    }
    nema_port_mutex_unlock(P.lock);
    return out;
}

size_t nema_proc_list(nema_proc_t *out, size_t max) {
    size_t n = 0;
    nema_port_mutex_lock(P.lock);
    for (int i = 0; i < NEMA_PROC_MAX && n < max; i++) {
        if (P.proc[i].state == NEMA_PROC_FREE) continue;
        out[n] = P.proc[i];
        memset(out[n].fd, 0, sizeof(out[n].fd));
        out[n].argv = NULL;
        n++;
    }
    nema_port_mutex_unlock(P.lock);
    return n;
}

/* ---- handle API ---- */
static nema_obj_t *get_fd(int fd) {
    nema_proc_t *p = nema_self();
    if (!p || fd < 0 || fd >= NEMA_HANDLES_MAX) return NULL;
    return p->fd[fd];
}

int nema_read_timeout(int fd, void *buf, size_t n, int32_t timeout_ms) {
    nema_obj_t *o = get_fd(fd);
    return o ? nema_obj_read(o, buf, n, timeout_ms) : NEMA_ERR_BADF;
}
int nema_read(int fd, void *buf, size_t n) { return nema_read_timeout(fd, buf, n, -1); }

int nema_write(int fd, const void *buf, size_t n) {
    nema_obj_t *o = get_fd(fd);
    return o ? nema_obj_write(o, buf, n, -1) : NEMA_ERR_BADF;
}

int nema_close(int fd) {
    nema_proc_t *p = nema_self();
    if (!p || fd < 0 || fd >= NEMA_HANDLES_MAX || !p->fd[fd]) return NEMA_ERR_BADF;
    nema_obj_t *o = p->fd[fd];
    p->fd[fd] = NULL;
    nema_obj_unref(o);
    return NEMA_OK;
}

int nema_handle_install(nema_obj_t *o) {
    nema_proc_t *p = nema_self();
    if (!p || !o) return NEMA_ERR_INVAL;
    for (int i = 0; i < NEMA_HANDLES_MAX; i++) {
        if (!p->fd[i]) {
            p->fd[i] = nema_obj_ref(o);
            return i;
        }
    }
    return NEMA_ERR_FULL;
}

nema_obj_t *nema_handle_obj(int fd) { return get_fd(fd); }

int nema_dup(int fd) {
    nema_obj_t *o = get_fd(fd);
    return o ? nema_handle_install(o) : NEMA_ERR_BADF;
}

nema_err_t nema_dup2(int oldfd, int newfd) {
    nema_proc_t *p = nema_self();
    nema_obj_t *o = get_fd(oldfd);
    if (!p || !o || newfd < 0 || newfd >= NEMA_HANDLES_MAX) return NEMA_ERR_BADF;
    if (oldfd == newfd) return NEMA_OK;
    nema_obj_t *prev = p->fd[newfd];
    p->fd[newfd] = nema_obj_ref(o);
    nema_obj_unref(prev);
    return NEMA_OK;
}

nema_err_t nema_ioctl(int fd, uint32_t req, void *arg) {
    nema_obj_t *o = get_fd(fd);
    return o ? o->ops->ioctl(o, req, arg) : NEMA_ERR_BADF;
}

typedef struct {
    nema_poll_obj_t objs[NEMA_WAIT_OBJS_MAX];
    nema_pollfd_t *fds;
    size_t n;
    int ready;
    bool signalled;
    nema_proc_t *self;
} poll_h_t;

static bool pred_poll_h(void *c) {
    poll_h_t *h = (poll_h_t *)c;
    int ready = 0;
    for (size_t i = 0; i < h->n; i++) {
        nema_obj_t *o = h->objs[i].obj;
        uint32_t ev = h->objs[i].events;
        uint32_t r = o ? o->ops->poll(o, ev) : NEMA_POLL_ERR;
        r &= ev | NEMA_POLL_HUP | NEMA_POLL_ERR;
        h->fds[i].revents = r;
        if (r) ready++;
    }
    h->ready = ready;
    if (ready) return true;
    /* a non-fatal signal (e.g. window resize) also ends the wait so the app can react */
    if (h->self && (h->self->sigpending & ~NEMA_SIG_FATAL)) {
        h->signalled = true;
        return true;
    }
    return false;
}

int nema_poll(nema_pollfd_t *fds, size_t n, int32_t timeout_ms) {
    if (n > NEMA_WAIT_OBJS_MAX) return NEMA_ERR_INVAL;
    poll_h_t h;
    memset(&h, 0, sizeof(h));
    h.fds = fds;
    h.n = n;
    h.self = nema_self();
    nema_obj_t *objs[NEMA_WAIT_OBJS_MAX];
    size_t no = 0;
    for (size_t i = 0; i < n; i++) {
        h.objs[i].obj = get_fd(fds[i].fd);
        h.objs[i].events = fds[i].events;
        fds[i].revents = 0;
        if (h.objs[i].obj) objs[no++] = h.objs[i].obj;
    }
    int w = nema_wait_on(objs, no, pred_poll_h, &h, timeout_ms);
    if (w == NEMA_OK) return h.signalled && !h.ready ? NEMA_ERR_INTR : h.ready;
    if (w == NEMA_ERR_TIMEOUT) return 0;
    return w;
}
