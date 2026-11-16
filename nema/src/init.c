#include "internal.h"

void nema_init(const nema_config_t *cfg) {
    nema_config_t c = cfg ? *cfg : (nema_config_t){0};
    nema_mem_init(c.internal_floor);
    nema_log_init(c.log_ring_bytes, c.console_log);
    nema_task_init();
    nema_wait_init();
    nema_registry_init();
    nema_proc_init();
    nema_vfs_init();
    NEMA_LOGI("nema", "kernel up, internal floor %zu KB", nema_mem_floor() / 1024);
}
