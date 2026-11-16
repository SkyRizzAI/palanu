/* The board's side of the boot: the services only the ESP port has (link-uart, time, ota-confirm), the radio glue, and the
 * boot-attempt counter in NVS. targets/hhv2/main/main.c hands these to pal_boot through pal_boot_cfg_t. */
#ifndef PAL_SERVICES_ESP_H
#define PAL_SERVICES_ESP_H

#include "pal/bootcount.h"

/* pal_boot_cfg_t.platform_services: radio glue and OTA ops, then the services link-uart, time, ota-confirm. */
void pal_esp_register_services(void);

/* pal_boot_cfg_t.bootcount: the counter in NVS (namespace "sys", key "bootcnt"). */
const pal_bootcount_ops_t *pal_bootcount_esp(void);

#endif
