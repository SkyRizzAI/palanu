/* The LVGL pool hook (lv_conf.h): Athena takes the pool at its start and gives it back at its stop (athena.c). */
#ifndef ATHENA_POOL_H
#define ATHENA_POOL_H

#include <stddef.h>

void *athena_pool(size_t size);

#endif
