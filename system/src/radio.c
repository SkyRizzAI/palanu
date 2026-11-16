/* PalanuOS radio manager (docs/RADIO.md). One lock guards the state of every radio; the board's start and stop run
 * with the lock released, and a radio in STARTING or STOPPING makes other callers wait, so status reads never stall.
 *
 * Grace period: the last release arms a deadline. A service task (the reaper) stops the radio once the deadline passes
 * and nobody has asked for it again. The task exists only while some radio is armed, so an idle system keeps no
 * stack for it.
 */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/radio.h"
#include "pal/sysinfo.h"

/* Expected internal RAM while each radio is on. Start values; the orchestrator replaces them with the numbers measured
 * on the board (docs/SIZE-BUDGETS.md, "radio" rows) and records the change there. */
#define RADIO_COST_WIFI_KB 42 /* measured on hhv2: 39 KB started and not connected; connecting adds buffers, see docs/SIZE-BUDGETS.md */
#define RADIO_COST_BLE_KB 38 /* measured on hhv2: 37 KB advertising */

/* Admission (docs/RADIO.md): after the start, this much internal RAM must still be free ... */
#define RADIO_FLOOR_KB 32
/* ... and the largest free block must be at least min(cost / 2, this). */
#define RADIO_BLOCK_MAX_KB 24

#define RADIO_GRACE_MS_DEFAULT 10000u
#define RADIO_REAPER_TICK_MS 100u
#define RADIO_REAPER_STACK 3072u
/* The board's start and stop (esp_wifi_init, esp_wifi_deinit, the NimBLE host, esp_netif) need far more stack than the
 * callers have: the service supervisor task has 3 KB. They run on a task of their own that exists only for the call
 * (found on the board: the supervisor overflowed its stack on `wifi off` and corrupted the heap around it). */
#define RADIO_OP_STACK 8192u
#define RADIO_WAIT_STEP_MS 50u
#define RADIO_WAIT_MAX_MS 30000u /* a start or stop that takes longer than this makes the caller give up */
#define RADIO_SETTLE_MS_DEFAULT 1200u /* between operations on different radios (pal_radio_set_settle_ms) */

typedef struct {
    pal_radio_state_t state;
    uint8_t n;
    char holders[PAL_RADIO_HOLDERS][PAL_RADIO_HOLDER_LEN];
    bool grace_pending;
    uint64_t grace_deadline_us;
    char error[PAL_RADIO_ERR_LEN];
} slot_t;

static struct {
    nema_port_mutex_t *lock; /* created on first use; the first use is at boot, before any task runs */
    nema_port_mutex_t *op_lock; /* one start or stop at a time, across both radios (run_op) */
    uint64_t op_end_us;         /* when the last operation finished; guarded by op_lock */
    int op_last;                /* which radio it was on, -1 before any */
    uint32_t settle_ms;
    slot_t r[PAL_RADIO_COUNT];
    const pal_radio_ops_t *ops;
    pal_radio_mem_fn mem;
    uint32_t grace_ms;
    bool reaper; /* the reaper task is alive (or about to be); guarded by the lock */
} R = {.grace_ms = RADIO_GRACE_MS_DEFAULT, .op_last = -1, .settle_ms = RADIO_SETTLE_MS_DEFAULT};

static void lock(void) { nema_port_mutex_lock(R.lock); }
static void unlock(void) { nema_port_mutex_unlock(R.lock); }

static bool valid(pal_radio_t r) { return (int)r >= 0 && r < PAL_RADIO_COUNT; }

static bool holder_ok(const char *h) { return h && h[0] && strlen(h) < PAL_RADIO_HOLDER_LEN; }

static int find_holder(const slot_t *s, const char *h) {
    for (int i = 0; i < PAL_RADIO_HOLDERS; i++)
        if (s->holders[i][0] && strcmp(s->holders[i], h) == 0) return i;
    return -1;
}

static void add_holder(slot_t *s, const char *h) {
    for (int i = 0; i < PAL_RADIO_HOLDERS; i++) {
        if (!s->holders[i][0]) {
            nema_strlcpy(s->holders[i], h, PAL_RADIO_HOLDER_LEN);
            s->n++;
            return;
        }
    }
}

static void del_holder(slot_t *s, int i) {
    s->holders[i][0] = 0;
    s->n--;
}

static void set_error(slot_t *s, const char *msg) { nema_strlcpy(s->error, msg, sizeof(s->error)); }

/* ---- costs and admission ---- */

size_t pal_radio_cost_kb(pal_radio_t r) {
    switch (r) {
    case PAL_RADIO_WIFI: return RADIO_COST_WIFI_KB;
    case PAL_RADIO_BLE: return RADIO_COST_BLE_KB;
    default: return 0;
    }
}

const char *pal_radio_name(pal_radio_t r) {
    switch (r) {
    case PAL_RADIO_WIFI: return "wifi";
    case PAL_RADIO_BLE: return "ble";
    default: return "?";
    }
}

const char *pal_radio_state_name(pal_radio_state_t s) {
    switch (s) {
    case PAL_RADIO_OFF: return "off";
    case PAL_RADIO_STARTING: return "starting";
    case PAL_RADIO_ON: return "on";
    case PAL_RADIO_STOPPING: return "stopping";
    case PAL_RADIO_FAILED: return "failed";
    default: return "?";
    }
}

/* Lock held. Refuses with NEMA_ERR_NOMEM and a message when the start would leave too little internal RAM. */
static nema_err_t admit(pal_radio_t r, slot_t *s) {
    size_t free_b = 0, largest = 0;
    if (R.mem) R.mem(&free_b, &largest);
    else pal_sys_mem_stats(PAL_MEM_INTERNAL, &free_b, NULL, &largest);
    size_t cost = pal_radio_cost_kb(r) * 1024u;
    size_t need_free = cost + RADIO_FLOOR_KB * 1024u;
    size_t block_max = RADIO_BLOCK_MAX_KB * 1024u;
    size_t need_block = cost / 2 < block_max ? cost / 2 : block_max;
    if (free_b >= need_free && largest >= need_block) return NEMA_OK;
    char msg[PAL_RADIO_ERR_LEN];
    snprintf(msg, sizeof(msg), "%s refused: needs %luK free and a %luK block, have %luK and %luK; free memory or leave it off",
             pal_radio_name(r), (unsigned long)(need_free / 1024), (unsigned long)(need_block / 1024),
             (unsigned long)(free_b / 1024), (unsigned long)(largest / 1024));
    set_error(s, msg);
    return NEMA_ERR_NOMEM;
}

/* ---- stop ---- */

typedef struct {
    bool start;
    pal_radio_t r;
    nema_err_t rc;
    volatile bool finished;
} op_t;

static void op_task(void *arg) {
    op_t *o = (op_t *)arg;
    if (!R.ops) o->rc = NEMA_ERR_NOSYS;
    else if (o->start) o->rc = R.ops->start ? R.ops->start(o->r) : NEMA_ERR_NOSYS;
    else o->rc = R.ops->stop ? R.ops->stop(o->r) : NEMA_OK;
    o->finished = true;
}

/* Lock not held. Runs the board's start or stop on a short-lived task with a big stack and waits for it. */
static nema_err_t run_op(bool start, pal_radio_t r) {
    if (!R.op_lock) R.op_lock = nema_port_mutex_create(); /* first use is at boot, before the radios have two callers */
    nema_port_mutex_lock(R.op_lock);
    /* The other radio changed a moment ago: let it settle first (pal_radio_set_settle_ms). */
    if (R.op_last >= 0 && R.op_last != (int)r && R.settle_ms > 0) {
        uint64_t due = R.op_end_us + (uint64_t)R.settle_ms * 1000u, now = pal_sys_uptime_us();
        if (now < due) nema_sleep_ms((uint32_t)((due - now) / 1000u) + 1u);
    }
    op_t o = {.start = start, .r = r, .rc = NEMA_OK, .finished = false};
    nema_task_desc_t td = {.name = "radio-op", .fn = op_task, .arg = &o, .stack = RADIO_OP_STACK, .prio = 3,
                           .core = -1, .role = NEMA_TASK_SERVICE, .owner = nema_owner_register("radio")};
    nema_err_t e = nema_task_create(&td);
    if (e != NEMA_OK) {
        nema_port_mutex_unlock(R.op_lock);
        return e == NEMA_ERR_NOMEM ? NEMA_ERR_NOMEM : e;
    }
    while (!o.finished) nema_sleep_ms(5); /* a start or stop takes hundreds of milliseconds: polling is fine */
    R.op_end_us = pal_sys_uptime_us();
    R.op_last = (int)r;
    nema_port_mutex_unlock(R.op_lock);
    return o.rc;
}

/* Lock held. ON or FAILED moves to STOPPING and returns true: the caller must then call run_stop() without the lock. */
static bool begin_stop(slot_t *s) {
    if (s->state != PAL_RADIO_ON && s->state != PAL_RADIO_FAILED) return false;
    s->state = PAL_RADIO_STOPPING;
    s->grace_pending = false;
    return true;
}

/* Lock not held. Runs the board's stop for a radio that begin_stop() moved to STOPPING. */
static void run_stop(pal_radio_t r) {
    nema_err_t rc = R.ops && R.ops->stop ? run_op(false, r) : NEMA_OK;
    lock();
    slot_t *s = &R.r[r];
    s->state = PAL_RADIO_OFF;
    if (rc != NEMA_OK) {
        char msg[PAL_RADIO_ERR_LEN];
        snprintf(msg, sizeof(msg), "%s stop failed (error %d)", pal_radio_name(r), (int)rc);
        set_error(s, msg);
    }
    unlock();
}

/* ---- reaper: stops radios whose grace period has passed ---- */

static void reaper(void *arg) {
    (void)arg;
    for (;;) {
        nema_sleep_ms(RADIO_REAPER_TICK_MS);
        for (int i = 0; i < PAL_RADIO_COUNT; i++) {
            lock();
            slot_t *s = &R.r[i];
            bool go = false;
            if (s->grace_pending) {
                if (s->n > 0 || (s->state != PAL_RADIO_ON && s->state != PAL_RADIO_STARTING)) s->grace_pending = false;
                else if (s->state == PAL_RADIO_ON && pal_sys_uptime_us() >= s->grace_deadline_us) go = begin_stop(s);
            }
            unlock();
            if (go) run_stop((pal_radio_t)i);
        }
        lock();
        bool pending = R.r[0].grace_pending || R.r[1].grace_pending;
        if (!pending) R.reaper = false; /* checked and cleared under the lock, so arm_grace() cannot miss it */
        unlock();
        if (!pending) return;
    }
}

static bool start_reaper(void) {
    nema_task_desc_t td = {.name = "radio", .fn = reaper, .arg = NULL, .stack = RADIO_REAPER_STACK, .prio = 2,
                           .core = -1, .role = NEMA_TASK_SERVICE, .owner = nema_owner_register("radio")};
    return nema_task_create(&td) == NEMA_OK;
}

/* Lock held, no holders left. Arms the grace deadline. Returns true when the radio must stop at once instead
 * (grace 0, or no task could be started to wait for the deadline); the caller then calls begin_stop + run_stop. */
static bool arm_grace(slot_t *s) {
    if (R.grace_ms == 0 && s->state == PAL_RADIO_ON) return true;
    s->grace_pending = true;
    s->grace_deadline_us = pal_sys_uptime_us() + (uint64_t)R.grace_ms * 1000u;
    if (R.reaper) return false;
    if (start_reaper()) {
        R.reaper = true;
        return false;
    }
    s->grace_pending = false;
    return s->state == PAL_RADIO_ON;
}

/* ---- public API ---- */

void pal_radio_set_ops(const pal_radio_ops_t *ops) {
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    R.ops = ops;
    unlock();
}

void pal_radio_set_settle_ms(uint32_t ms) { R.settle_ms = ms; }

nema_err_t pal_radio_request(pal_radio_t r, const char *holder) {
    if (!valid(r) || !holder_ok(holder)) return NEMA_ERR_INVAL;
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    slot_t *s = &R.r[r];
    uint32_t waited = 0;
    while (s->state == PAL_RADIO_STARTING || s->state == PAL_RADIO_STOPPING) {
        if (waited >= RADIO_WAIT_MAX_MS) {
            set_error(s, "timed out waiting for the radio to change state");
            unlock();
            return NEMA_ERR_TIMEOUT;
        }
        unlock();
        nema_sleep_ms(RADIO_WAIT_STEP_MS);
        waited += RADIO_WAIT_STEP_MS;
        lock();
    }
    if (find_holder(s, holder) >= 0) {
        unlock();
        return NEMA_OK;
    }
    if (s->n >= PAL_RADIO_HOLDERS) {
        set_error(s, "too many holders");
        unlock();
        return NEMA_ERR_FULL;
    }
    if (s->state == PAL_RADIO_ON) {
        add_holder(s, holder);
        s->grace_pending = false;
        unlock();
        return NEMA_OK;
    }
    if (!R.ops || !R.ops->start) {
        set_error(s, "no driver for this radio on this board");
        unlock();
        return NEMA_ERR_NOSYS;
    }
    nema_err_t a = admit(r, s);
    if (a != NEMA_OK) {
        unlock();
        return a;
    }
    add_holder(s, holder);
    s->grace_pending = false;
    s->error[0] = 0;
    s->state = PAL_RADIO_STARTING;
    unlock();

    nema_err_t rc = run_op(true, r);

    lock();
    bool go = false;
    if (rc == NEMA_OK) {
        s->state = PAL_RADIO_ON;
        s->error[0] = 0;
        /* released while it was starting: nobody holds it any more */
        if (s->n == 0 && arm_grace(s)) go = begin_stop(s);
        unlock();
        if (go) run_stop(r);
        return NEMA_OK;
    }
    /* A failed start may have left parts up. Stop is called to take them down (it must work after a failed start);
     * STOPPING keeps other callers waiting meanwhile, and the state ends as FAILED so the next request retries. */
    int i = find_holder(s, holder);
    if (i >= 0) del_holder(s, i);
    s->grace_pending = false;
    char msg[PAL_RADIO_ERR_LEN];
    snprintf(msg, sizeof(msg), "%s failed to start (error %d: %s)", pal_radio_name(r), (int)rc, nema_err_str(rc));
    set_error(s, msg);
    s->state = PAL_RADIO_STOPPING;
    unlock();
    if (R.ops->stop) (void)run_op(false, r);
    lock();
    s->state = PAL_RADIO_FAILED;
    unlock();
    return rc;
}

nema_err_t pal_radio_release(pal_radio_t r, const char *holder) {
    if (!valid(r) || !holder_ok(holder)) return NEMA_ERR_INVAL;
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    slot_t *s = &R.r[r];
    int i = find_holder(s, holder);
    if (i < 0) {
        unlock();
        return NEMA_ERR_NOENT;
    }
    del_holder(s, i);
    bool go = false;
    if (s->n == 0 && (s->state == PAL_RADIO_ON || s->state == PAL_RADIO_STARTING)) {
        if (arm_grace(s)) go = begin_stop(s);
    }
    unlock();
    if (go) run_stop(r);
    return NEMA_OK;
}

void pal_radio_force_off(pal_radio_t r) {
    if (!valid(r)) return;
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    slot_t *s = &R.r[r];
    for (int i = 0; i < PAL_RADIO_HOLDERS; i++) s->holders[i][0] = 0;
    s->n = 0;
    s->grace_pending = false;
    while (s->state == PAL_RADIO_STARTING || s->state == PAL_RADIO_STOPPING) {
        unlock();
        nema_sleep_ms(RADIO_WAIT_STEP_MS);
        lock();
    }
    bool go = begin_stop(s);
    unlock();
    if (go) run_stop(r);
}

pal_radio_state_t pal_radio_state(pal_radio_t r) {
    if (!valid(r)) return PAL_RADIO_OFF;
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    pal_radio_state_t st = R.r[r].state;
    unlock();
    return st;
}

void pal_radio_info(pal_radio_t r, pal_radio_info_t *out) {
    memset(out, 0, sizeof(*out));
    if (!valid(r)) return;
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    const slot_t *s = &R.r[r];
    out->state = s->state;
    out->nholders = s->n;
    memcpy(out->holders, s->holders, sizeof(out->holders));
    out->grace_pending = s->grace_pending;
    memcpy(out->error, s->error, sizeof(out->error));
    unlock();
}

const char *pal_radio_last_error(pal_radio_t r) { return valid(r) ? R.r[r].error : ""; }

size_t pal_radio_detail(pal_radio_t r, char *out, size_t cap) {
    if (!cap) return 0;
    out[0] = 0;
    if (!valid(r) || !R.ops || !R.ops->detail) return 0;
    return R.ops->detail(r, out, cap);
}

void pal_radio_set_mem_source(pal_radio_mem_fn fn) {
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    R.mem = fn;
    unlock();
}

void pal_radio_set_grace_ms(uint32_t ms) {
    if (!R.lock) R.lock = nema_port_mutex_create();
    lock();
    R.grace_ms = ms;
    unlock();
}
