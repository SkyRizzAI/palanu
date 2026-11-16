/* The memory card, as the system sees it: mounted into the namespace at /sd, or not. The board supplies the driver
 * (pal_sd_ops_t: boards/hhv2/hhv2_sd.c; the simulator uses a host directory); everything else (Settings > Storage, Files,
 * the `sd` command) asks here, so there is one answer to "is there a card".
 *
 * There is no card-detect line on hhv2: a card is found by trying to mount it, at boot and on request. */
#ifndef PAL_SD_H
#define PAL_SD_H

#include <stdbool.h>
#include <stdint.h>

#include "nema/types.h"

#define PAL_SD_PATH "/sd"

typedef struct {
    char name[16];       /* the card's product name (CID), "" when unknown */
    uint64_t size_bytes; /* the card's capacity */
    uint32_t speed_khz;  /* the bus clock it runs at */
} pal_sd_info_t;

typedef struct {
    const char *(*mount)(void); /* mounts the card; returns its directory in the platform's filesystem, or NULL (no card) */
    void (*unmount)(void);      /* flushes and releases the card; safe when none is mounted */
    bool (*info)(pal_sd_info_t *out); /* false when no card is mounted; may be NULL */
    /* total and free bytes of the mounted card; may be NULL (then the filesystem's statfs is asked). FAT on ESP-IDF has
     * no statvfs, so the board answers this itself. */
    bool (*space)(uint64_t *total, uint64_t *free_bytes);
} pal_sd_ops_t;

void pal_sd_set_ops(const pal_sd_ops_t *ops); /* at boot; NULL: the board has no card slot */
bool pal_sd_present(void);                    /* the board has a card slot (ops were given) */

/* Mounts the card at /sd. NEMA_OK (also when already mounted), NEMA_ERR_NOSYS without a slot, NEMA_ERR_NOENT when no
 * card could be mounted. */
nema_err_t pal_sd_mount(void);
/* Takes the card out of the namespace and releases it, so it can be pulled out. NEMA_OK also when none was mounted.
 * Files still open on the card fail from then on. */
nema_err_t pal_sd_eject(void);

bool pal_sd_mounted(void);
bool pal_sd_info(pal_sd_info_t *out); /* false when no card is mounted */
/* Space on the mounted card. False when no card is mounted. */
bool pal_sd_space(uint64_t *total, uint64_t *free_bytes);

#endif
