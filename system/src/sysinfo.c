/* PalanuOS system readings: thin wrappers over the port, see pal/sysinfo.h. */
#include "pal/sysinfo.h"

#include <string.h>

#include "nema/port.h"

_Static_assert(sizeof(pal_cpu_sample_t) == sizeof(nema_cpu_sample_t), "pal_cpu_sample_t must mirror nema_cpu_sample_t");
_Static_assert(PAL_CPU_MAX == NEMA_CPU_MAX, "pal and nema disagree on the CPU count");

static nema_mem_class_t mem_class(pal_mem_region_t r) { return r == PAL_MEM_EXTERNAL ? NEMA_MEM_EXTERNAL : NEMA_MEM_INTERNAL; }

int pal_sys_cpu_count(void) { return nema_port_cpu_count(); }

int pal_sys_cpu_percent(pal_cpu_sample_t *s, int core) {
    nema_cpu_sample_t n;
    memcpy(&n, s, sizeof(n));
    int pct = nema_port_cpu_percent(&n, core);
    memcpy(s, &n, sizeof(n));
    return pct;
}

size_t pal_sys_mem_total(pal_mem_region_t r) { return nema_port_mem_total(mem_class(r)); }
size_t pal_sys_mem_free(pal_mem_region_t r) { return nema_port_mem_free(mem_class(r)); }
void pal_sys_mem_stats(pal_mem_region_t r, size_t *free_bytes, size_t *min_free, size_t *largest) {
    nema_port_mem_stats((int)mem_class(r), free_bytes, min_free, largest);
}
uint64_t pal_sys_uptime_us(void) { return nema_port_time_us(); }

bool pal_sys_heap_check(void) { return nema_port_heap_check(); }
