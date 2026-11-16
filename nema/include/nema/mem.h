/* Nema memory: classed, owned, accounted allocation.
 *
 * Why: the predecessor died of internal-RAM starvation. Every allocation here names a class and an owner,
 * so "who holds the internal RAM" is always answerable, and the internal floor keeps radios and DMA alive.
 */
#ifndef NEMA_MEM_H
#define NEMA_MEM_H

#include "nema/port.h"

/* Owner ids fit the 6-bit owner field of the block header: 0 = kernel, 1..31 subsystems (nema_owner_register),
 * 32..63 process slots (one per NEMA_PROC_MAX, set by proc.c). Keep NEMA_PROC_MAX <= NEMA_OWNER_MAX - BASE. */
typedef uint8_t nema_owner_t;
#define NEMA_OWNER_KERNEL 0
#define NEMA_OWNER_MAX 64
#define NEMA_OWNER_PROC_BASE 32

#define NEMA_ALLOC_ZERO 0x1u     /* zero the block */
#define NEMA_ALLOC_CRITICAL 0x2u /* may dip below the internal floor (boot, radio, panic paths) */

/* Initial floor of INTERNAL/DMA free memory that ordinary allocations and task creation must leave alone. */
#define NEMA_MEM_FLOOR_DEFAULT (32u * 1024u)

nema_owner_t nema_owner_register(const char *name); /* returns existing id for a known name; 0 if full */
void nema_owner_set_name(nema_owner_t owner, const char *name);

void *nema_alloc_ex(nema_mem_class_t cls, nema_owner_t owner, size_t size, size_t align, uint32_t flags);
static inline void *nema_alloc(nema_mem_class_t cls, nema_owner_t owner, size_t size) {
    return nema_alloc_ex(cls, owner, size, 0, 0);
}
static inline void *nema_calloc(nema_mem_class_t cls, nema_owner_t owner, size_t size) {
    return nema_alloc_ex(cls, owner, size, 0, NEMA_ALLOC_ZERO);
}
void nema_free(void *p); /* NULL is fine */
size_t nema_alloc_size(const void *p);

/* Admission, in order, all under one lock: the owner's quota (process owners only), the internal floor (INTERNAL and
 * DMA, not CRITICAL), and for requests of 4 KB or more the largest free block (not CRITICAL). A refusal of floor,
 * block, or heap space runs the reclaimers once and tries again; a quota refusal never reclaims. NULL on refusal. */

/* Called when an allocation would breach the floor or the heap is exhausted. Return bytes released. */
typedef size_t (*nema_reclaim_fn)(void *ctx, size_t wanted);
nema_err_t nema_mem_add_reclaimer(nema_reclaim_fn fn, void *ctx, int priority); /* lower priority runs first */

void nema_mem_set_floor(size_t bytes);
size_t nema_mem_floor(void);
bool nema_mem_internal_can_spare(size_t bytes); /* would `bytes` more internal use keep us above the floor? */

/* Stack bytes are internal memory held by a task; they are attributed to the task's owner. */
void nema_mem_stack_account(nema_owner_t owner, size_t bytes, bool add);

typedef struct {
    char name[16];
    uint32_t used[NEMA_MEM_CLASS_COUNT]; /* bytes now, by actual class (ANY never appears) */
    uint32_t stack;                      /* task stack bytes */
    uint32_t peak;                       /* peak of all heap bytes */
    uint32_t allocs;                     /* live allocation count */
    uint32_t fails;                      /* refused allocations */
    uint32_t quota_ext;                  /* process owners: EXTERNAL bytes allowed; 0 = no quota */
    uint32_t quota_int;                  /* process owners: INTERNAL+DMA bytes allowed; 0 = no quota */
} nema_owner_stat_t;

bool nema_mem_owner_stat(nema_owner_t owner, nema_owner_stat_t *out); /* false for unused slots */
size_t nema_mem_report(char *buf, size_t cap);                         /* human-readable table; returns length */
/* Per-class free / min-free / largest block, and per-owner bytes in each class. Returns length. */
size_t nema_mem_report_ex(char *buf, size_t cap);

#endif
