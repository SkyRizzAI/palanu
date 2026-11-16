#include <string.h>

#include "internal.h"
#include "nema/log.h"
#include "nema/obj.h"
#include "nema/task.h"

#define MAX_TASKS 48 /* 32 process tasks plus boot-time subsystem and service tasks */

typedef struct {
    bool used;
    char name[16];
    nema_task_role_t role;
    nema_owner_t owner;
    uint32_t stack;
    void (*fn)(void *);
    void *arg;
} slot_t;

static struct {
    nema_port_mutex_t *lock;
    slot_t slot[MAX_TASKS];
} T;

void nema_task_init(void);
void nema_task_init(void) {
    memset(&T, 0, sizeof(T));
    T.lock = nema_port_mutex_create();
}

static void trampoline(void *p) {
    slot_t *s = (slot_t *)p;
    s->fn(s->arg);
    size_t low = nema_port_task_stack_free();
    if (low < 192) NEMA_LOGW("task", "%s: stack nearly exhausted (%zu bytes spare)", s->name, low);
    nema_mem_stack_account(s->owner, s->stack, false);
    nema_tctx_release();
    nema_port_mutex_lock(T.lock);
    s->used = false;
    nema_port_mutex_unlock(T.lock);
    nema_port_task_exit();
}

nema_err_t nema_task_create(const nema_task_desc_t *d) {
    if (!d || !d->fn || !d->name) return NEMA_ERR_INVAL;
    size_t stack = d->stack < NEMA_TASK_STACK_MIN ? NEMA_TASK_STACK_MIN : d->stack;
    size_t need = stack + NEMA_TASK_OVERHEAD;
    /* Only the SYSTEM role (boot-time subsystems) is exempt from the floor. The stack stays reserved until the port
     * has taken it, so two creations racing for the same free bytes cannot both pass. */
    if (!nema_mem_task_reserve(need, d->role != NEMA_TASK_SYSTEM)) {
        NEMA_LOGW("task", "refused %s: %zu byte stack would breach the internal floor", d->name, stack);
        return NEMA_ERR_NOMEM;
    }
    slot_t *s = NULL;
    nema_port_mutex_lock(T.lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!T.slot[i].used) {
            s = &T.slot[i];
            s->used = true;
            break;
        }
    }
    nema_port_mutex_unlock(T.lock);
    if (!s) {
        nema_mem_task_release(need);
        return NEMA_ERR_FULL;
    }
    nema_strlcpy(s->name, d->name, sizeof(s->name));
    s->name[sizeof(s->name) - 1] = 0;
    s->role = d->role;
    s->owner = d->owner;
    s->stack = (uint32_t)stack;
    s->fn = d->fn;
    s->arg = d->arg;
    nema_mem_stack_account(d->owner, stack, true);
    nema_port_task_t *h;
    nema_err_t r = nema_port_task_create(&h, d->name, trampoline, s, stack, d->prio, d->core);
    nema_mem_task_release(need);
    if (r != NEMA_OK) {
        nema_mem_stack_account(d->owner, stack, false);
        nema_port_mutex_lock(T.lock);
        s->used = false;
        nema_port_mutex_unlock(T.lock);
        return r;
    }
    return NEMA_OK;
}

size_t nema_task_list(nema_task_info_t *out, size_t max) {
    size_t n = 0;
    nema_port_mutex_lock(T.lock);
    for (int i = 0; i < MAX_TASKS && n < max; i++) {
        if (!T.slot[i].used) continue;
        memcpy(out[n].name, T.slot[i].name, sizeof(out[n].name));
        out[n].role = T.slot[i].role;
        out[n].owner = T.slot[i].owner;
        out[n].stack = T.slot[i].stack;
        n++;
    }
    nema_port_mutex_unlock(T.lock);
    return n;
}

size_t nema_task_count(void) {
    size_t n = 0;
    nema_port_mutex_lock(T.lock);
    for (int i = 0; i < MAX_TASKS; i++) n += T.slot[i].used ? 1u : 0u;
    nema_port_mutex_unlock(T.lock);
    return n;
}
