/* Nema internals shared between kernel source files. Not part of the public API. */
#ifndef NEMA_INTERNAL_H
#define NEMA_INTERNAL_H

#include "nema/nema.h"

void nema_mem_init(size_t floor);
void nema_log_init(size_t ring_bytes, bool console);
void nema_task_init(void);
void nema_wait_init(void);
void nema_registry_init(void);
void nema_vfs_init(void);

/* Process owners: reset the per-run counters and set the quotas (before the process's first allocation). */
void nema_mem_owner_begin(nema_owner_t owner, size_t quota_ext, size_t quota_int);
/* Free every heap block the owner still holds. Returns the bytes freed; `blocks` (optional) gets the count. */
size_t nema_mem_owner_sweep(nema_owner_t owner, size_t *blocks);
/* Pipes and TTYs are shared by reference count across processes, so they never belong to a process owner. */
nema_owner_t nema_mem_shared_owner(nema_owner_t owner);
/* Internal bytes for a task stack being created: counted against the floor until the port has taken the stack.
 * `gated` false (SYSTEM role) skips the floor check but still reserves. Returns false when refused. */
bool nema_mem_task_reserve(size_t bytes, bool gated);
void nema_mem_task_release(size_t bytes);

#endif
