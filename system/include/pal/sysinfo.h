/* PalanuOS system readings for apps and the desktop: CPU load, memory and uptime.
 *
 * Why: apps are the top layer and may not call the port directly (docs/CODING-RULES.md). These wrappers are the
 * public face of the readings, so the port can change underneath without touching apps.
 */
#ifndef PAL_SYSINFO_H
#define PAL_SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PAL_MEM_INTERNAL = 0, /* scarce SRAM */
    PAL_MEM_EXTERNAL,     /* PSRAM; reads 0 on a board without it */
} pal_mem_region_t;

#define PAL_CPU_MAX 2
/* Caller-owned sample state for pal_sys_cpu_percent(). Zero-initialise it before the first call. The layout
 * mirrors the port's sample type; system/src/sysinfo.c checks the two match. */
typedef struct {
    uint32_t idle[PAL_CPU_MAX];
    int64_t t[PAL_CPU_MAX];
} pal_cpu_sample_t;

int pal_sys_cpu_count(void);
int pal_sys_cpu_percent(pal_cpu_sample_t *s, int core); /* busy share since this sample's previous call, 0..100 */
size_t pal_sys_mem_total(pal_mem_region_t r);            /* bytes */
size_t pal_sys_mem_free(pal_mem_region_t r);             /* bytes */
/* Free, low-water mark since boot and largest free block in one reading; NULL outputs are skipped. */
void pal_sys_mem_stats(pal_mem_region_t r, size_t *free_bytes, size_t *min_free, size_t *largest);
uint64_t pal_sys_uptime_us(void);

/* Check the block headers of every heap (board: a damaged block is printed on the console). true = intact. */
bool pal_sys_heap_check(void);

#endif
