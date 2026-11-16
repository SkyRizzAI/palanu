/* The crypto tables of a process (crypto_tab.h). Allocated on first use under the process owner, so the sweep at exit
 * frees them with everything else the process held. */
#include "crypto_tab.h"

#include "nema/mem.h"
#include "nema/proc.h"

static crypto_tab_t *s_kernel; /* calls from no process (kernel tasks, tests) */

/* A built-in command is a process too: its table is charged to it and freed by the sweep when it exits. Its slot is
 * keyed by the process slot (owner id); start_us tells a new process from the one that held the slot before. */
typedef struct {
    uint64_t start_us;
    nema_pid_t pid;
    crypto_tab_t *tab;
} native_slot_t;
static native_slot_t s_native[NEMA_PROC_MAX];

static crypto_tab_t *alloc_tab(nema_owner_t owner) { return nema_calloc(NEMA_MEM_ANY, owner, sizeof(crypto_tab_t)); }

crypto_tab_t *crypto_tab(api_ctx_t *c) {
    if (c) {
        void **slot = &api_ext(c)->crypto;
        nema_proc_t *self = nema_self();
        if (!*slot) *slot = alloc_tab(self ? self->owner : NEMA_OWNER_KERNEL);
        return (crypto_tab_t *)*slot;
    }
    nema_proc_t *p = nema_self();
    if (!p) {
        if (!s_kernel) s_kernel = alloc_tab(NEMA_OWNER_KERNEL);
        return s_kernel;
    }
    int i = (int)p->owner - NEMA_OWNER_PROC_BASE;
    if (i < 0 || i >= NEMA_PROC_MAX) return NULL;
    native_slot_t *s = &s_native[i];
    if (!s->tab || s->start_us != p->start_us || s->pid != p->pid) {
        s->tab = alloc_tab(p->owner);
        s->start_us = p->start_us;
        s->pid = p->pid;
    }
    return s->tab;
}
