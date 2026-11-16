/* Nema, the PalanuOS kernel: memory, tasks, processes, handles, and TTYs. */
#ifndef NEMA_H
#define NEMA_H

#include "nema/log.h"
#include "nema/mem.h"
#include "nema/net.h"
#include "nema/obj.h"
#include "nema/pipe.h"
#include "nema/proc.h"
#include "nema/registry.h"
#include "nema/task.h"
#include "nema/tty.h"
#include "nema/vfs.h"

typedef struct {
    size_t internal_floor; /* 0 = NEMA_MEM_FLOOR_DEFAULT */
    size_t log_ring_bytes; /* 0 = 16 KB */
    bool console_log;
} nema_config_t;

/* Boot order inside: memory, log, wait, processes. Call once, from the first task. */
void nema_init(const nema_config_t *cfg);

#endif
