/* Boot-attempt counter and safe mode (system/include/pal/bootcount.h). The platform stores the number; this file
 * decides when it goes up, when it is cleared, and whether this boot is safe. */
#include "pal/bootcount.h"

#include <stdio.h>

static struct {
    const pal_bootcount_ops_t *ops;
    uint32_t value;
    bool safe;
    bool ready_seen;
    bool cleared;
    uint64_t ready_at_us;
} B;

bool psys_bootcount_begin(const pal_bootcount_ops_t *ops) {
    B.ops = ops;
    B.value = 0;
    B.safe = false;
    B.ready_seen = false;
    B.cleared = false;
    if (!ops || !ops->get || !ops->set) return false;
    uint32_t n = ops->get() + 1u;
    if (ops->set(n) != NEMA_OK) NEMA_LOGW("boot", "boot counter not stored; safe mode cannot be detected");
    B.value = n;
    B.safe = n >= PSYS_SAFE_BOOT_TRIES;
    if (B.safe) NEMA_LOGE("boot", "SAFE MODE: %lu boots in a row did not reach ready", (unsigned long)n);
    return B.safe;
}

bool psys_bootcount_safe(void) { return B.safe; }
uint32_t psys_bootcount_value(void) { return B.value; }
bool psys_bootcount_is_ready(void) { return B.ready_seen; }

void psys_bootcount_ready_check(uint64_t now_us, bool settled) {
    if (!B.ops || B.cleared || B.safe) return; /* safe mode keeps the count until `safemode off` */
    if (!B.ready_seen) {
        if (!settled) return;
        B.ready_seen = true;
        B.ready_at_us = now_us;
        NEMA_LOGI("boot", "ready; the boot counter clears in %lu s", (unsigned long)(PSYS_BOOT_READY_CLEAR_MS / 1000u));
        return;
    }
    if (now_us - B.ready_at_us < (uint64_t)PSYS_BOOT_READY_CLEAR_MS * 1000u) return;
    B.cleared = true;
    if (B.ops->set(0) == NEMA_OK) NEMA_LOGI("boot", "boot counter cleared: this boot is healthy");
    else NEMA_LOGW("boot", "boot counter could not be cleared");
}

nema_err_t psys_bootcount_clear(void) {
    if (!B.ops) return NEMA_ERR_NOSYS;
    nema_err_t r = B.ops->set(0);
    if (r == NEMA_OK) {
        B.value = 0;
        B.cleared = true;
    }
    return r;
}

void psys_bootcount_reset_for_test(void) {
    B.ops = NULL;
    B.value = 0;
    B.safe = false;
    B.ready_seen = false;
    B.cleared = false;
    B.ready_at_us = 0;
}
