/* Nema task gate: every task in the system is created here.
 *
 * Why: stacks are internal RAM, the scarcest resource. One gate means one place that knows every stack,
 * refuses new work when the floor would be breached, and can report who holds what.
 */
#ifndef NEMA_TASK_H
#define NEMA_TASK_H

#include "nema/mem.h"

typedef enum {
    NEMA_TASK_SYSTEM = 0, /* boot-time subsystem task; may dip below the floor */
    NEMA_TASK_PROCESS,    /* runs a process; refused when the floor would be breached */
    NEMA_TASK_SERVICE,    /* background service; refused (caller retries) when the floor would be breached */
} nema_task_role_t;

typedef struct {
    const char *name;
    void (*fn)(void *arg);
    void *arg;
    size_t stack; /* bytes, declared up front */
    uint8_t prio; /* 0 = lowest; the port maps this */
    int8_t core;  /* -1 = any */
    nema_task_role_t role;
    nema_owner_t owner;
} nema_task_desc_t;

#define NEMA_TASK_STACK_MIN 1536u
#define NEMA_TASK_OVERHEAD 1024u /* control block and bookkeeping assumed on top of the stack */

nema_err_t nema_task_create(const nema_task_desc_t *desc);

typedef struct {
    char name[16];
    nema_task_role_t role;
    nema_owner_t owner;
    uint32_t stack;
} nema_task_info_t;

size_t nema_task_list(nema_task_info_t *out, size_t max);
size_t nema_task_count(void);

#endif
