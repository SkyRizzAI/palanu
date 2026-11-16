/* The memory card in the namespace (system/include/pal/sd.h): the board's driver mounts the card in the platform's
 * filesystem, and this file puts that directory at /sd through the POSIX backend, as boot does for /home and /data. */
#include "pal/sd.h"

#include <string.h>

#include "nema/nema.h"
#include "nema/port.h"

static struct {
    const pal_sd_ops_t *ops;
    nema_port_mutex_t *lock;
    bool mounted;
    void *pfs;          /* the POSIX backend over the card's directory: made once, reused by every mount */
    char root[256];     /* that directory (the simulator's is a long host path) */
} S;

static void lock(void) {
    if (!__atomic_load_n(&S.lock, __ATOMIC_ACQUIRE)) {
        nema_port_mutex_t *m = nema_port_mutex_create();
        if (!m) nema_panic("sd: no mutex");
        nema_port_mutex_t *expect = NULL;
        if (!__atomic_compare_exchange_n(&S.lock, &expect, m, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            nema_port_mutex_destroy(m);
    }
    nema_port_mutex_lock(S.lock);
}

static void unlock(void) { nema_port_mutex_unlock(S.lock); }

void pal_sd_set_ops(const pal_sd_ops_t *ops) {
    lock();
    S.ops = ops;
    unlock();
}

bool pal_sd_present(void) {
    lock();
    bool p = S.ops != NULL;
    unlock();
    return p;
}

nema_err_t pal_sd_mount(void) {
    lock();
    nema_err_t r = NEMA_OK;
    if (!S.ops || !S.ops->mount) r = NEMA_ERR_NOSYS;
    else if (!S.mounted) {
        const char *root = S.ops->mount(); /* slow without a card (the card's start-up times out): fine, it is rare */
        if (!root) r = NEMA_ERR_NOENT;
        else {
            if (!S.pfs || strcmp(S.root, root) != 0) {
                nema_strlcpy(S.root, root, sizeof(S.root));
                S.pfs = nema_posixfs_create(S.root);
            }
            nema_fs_mkdir(PAL_SD_PATH); /* the mount point in the RAM root, so `ls /` shows it */
            r = S.pfs ? nema_vfs_mount(PAL_SD_PATH, nema_posixfs_ops(), S.pfs) : NEMA_ERR_NOMEM;
            if (r == NEMA_OK) S.mounted = true;
            else S.ops->unmount();
        }
    }
    unlock();
    if (r == NEMA_OK) NEMA_LOGI("sd", "card at %s", PAL_SD_PATH);
    return r;
}

nema_err_t pal_sd_eject(void) {
    lock();
    if (S.mounted) {
        nema_vfs_unmount(PAL_SD_PATH);
        if (S.ops && S.ops->unmount) S.ops->unmount();
        S.mounted = false;
    }
    unlock();
    return NEMA_OK;
}

bool pal_sd_mounted(void) {
    lock();
    bool m = S.mounted;
    unlock();
    return m;
}

bool pal_sd_info(pal_sd_info_t *out) {
    lock();
    bool ok = S.mounted && S.ops && S.ops->info && S.ops->info(out);
    unlock();
    return ok;
}

bool pal_sd_space(uint64_t *total, uint64_t *free_bytes) {
    lock();
    bool mounted = S.mounted;
    bool (*space)(uint64_t *, uint64_t *) = S.ops ? S.ops->space : NULL;
    unlock();
    if (!mounted) return false;
    if (space) return space(total, free_bytes);
    return nema_fs_statfs(PAL_SD_PATH, total, free_bytes) == NEMA_OK;
}
