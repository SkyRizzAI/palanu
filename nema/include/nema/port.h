/* Nema port: everything the kernel needs from the platform.
 *
 * Implemented in platform/esp-idf (FreeRTOS + heap_caps) and platform/host (pthreads).
 * Nothing above this header may include a platform header.
 */
#ifndef NEMA_PORT_H
#define NEMA_PORT_H

#include "nema/types.h"

/* Memory classes. INTERNAL is the scarce, fast SRAM; DMA is internal memory a DMA engine may touch;
 * EXTERNAL is PSRAM. ANY is resolved by the kernel (prefer EXTERNAL, fall back to INTERNAL). */
typedef enum {
    NEMA_MEM_INTERNAL = 0,
    NEMA_MEM_DMA,
    NEMA_MEM_EXTERNAL,
    NEMA_MEM_ANY,
    NEMA_MEM_CLASS_COUNT,
} nema_mem_class_t;

/* ---- memory (raw; kernel code uses nema_alloc) ---- */
void *nema_port_alloc(nema_mem_class_t cls, size_t size, size_t align); /* NULL on failure; align 0 = default */
void nema_port_free(nema_mem_class_t cls, void *p);
size_t nema_port_mem_total(nema_mem_class_t cls);   /* 0 if the class does not exist on this board */
size_t nema_port_mem_free(nema_mem_class_t cls);
size_t nema_port_mem_largest(nema_mem_class_t cls);
size_t nema_port_mem_min_free(nema_mem_class_t cls); /* low-water mark since boot */
/* Free, minimum-free-ever and largest-free-block in one call; NULL outputs are skipped. cls is an nema_mem_class_t. */
void nema_port_mem_stats(int cls, size_t *free, size_t *min_free, size_t *largest);

/* ---- time ---- */
uint64_t nema_port_time_us(void);
void nema_port_sleep_ms(uint32_t ms);

/* ---- randomness ---- */
/* Fills buf with random bytes from the chip's TRNG (ESP) or the host's entropy source. Never returns short. */
void nema_port_random(void *buf, size_t len);

/* ---- synchronisation ---- */
typedef struct nema_port_mutex nema_port_mutex_t;
typedef struct nema_port_sem nema_port_sem_t;

nema_port_mutex_t *nema_port_mutex_create(void);
void nema_port_mutex_lock(nema_port_mutex_t *m);
void nema_port_mutex_unlock(nema_port_mutex_t *m);
void nema_port_mutex_destroy(nema_port_mutex_t *m);

/* Counting semaphore with an upper bound. timeout_ms < 0 waits forever, 0 polls. */
nema_port_sem_t *nema_port_sem_create(uint32_t initial, uint32_t max);
nema_err_t nema_port_sem_take(nema_port_sem_t *s, int32_t timeout_ms); /* NEMA_OK or NEMA_ERR_TIMEOUT */
void nema_port_sem_give(nema_port_sem_t *s);
void nema_port_sem_destroy(nema_port_sem_t *s);

/* ---- tasks ---- */
typedef struct nema_port_task nema_port_task_t;
typedef void (*nema_port_task_fn)(void *arg);

/* core: -1 = any. The stack lives in internal memory on every platform. */
nema_err_t nema_port_task_create(nema_port_task_t **out, const char *name, nema_port_task_fn fn, void *arg,
                                 size_t stack_bytes, int prio, int core);
/* Walk every heap and check the block headers; on the board a damaged block is printed to the console. true = intact. */
bool nema_port_heap_check(void);
void nema_port_task_exit(void) __attribute__((noreturn)); /* terminate the calling task */
size_t nema_port_task_stack_free(void);                   /* high-water mark of the calling task, bytes */

/* One pointer of thread-local storage per task. */
void *nema_port_tls_get(void);
void nema_port_tls_set(void *p);

/* ---- diagnostics ---- */
#define NEMA_CPU_MAX 2
/* Previous-sample state for nema_port_cpu_percent(). The caller owns it, so each reader (top, Monitor) computes its
 * own deltas. Zero-initialise it before the first call; the first reading for a core is 0. */
typedef struct {
    uint32_t idle[NEMA_CPU_MAX]; /* port-specific idle counter at the last sample */
    int64_t t[NEMA_CPU_MAX];     /* time of the last sample, us; 0 = never sampled */
} nema_cpu_sample_t;

int nema_port_cpu_count(void);
int nema_port_cpu_percent(nema_cpu_sample_t *s, int core); /* busy share since this sample's previous call, 0..100 */

/* ---- console and abort ---- */
void nema_port_console_write(const char *buf, size_t n); /* raw bytes to the debug console */
void nema_port_abort(void) __attribute__((noreturn));

#endif
