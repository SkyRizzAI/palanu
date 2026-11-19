/* Power profiles (pal/power.h): each one is a CPU range in proportion to the board's top clock. */
#include "pal/power.h"

#include <stdio.h>

static pal_power_ops_t P;
static bool s_have;
static int s_profile = PAL_POWER_BALANCED;
static nema_err_t s_last = NEMA_ERR_NOSYS;
static int s_min, s_max;

static const char *const NAMES[] = {"Performance", "Balanced", "Saver"};

void pal_power_set_ops(const pal_power_ops_t *ops) {
    if (!ops) return;
    P = *ops;
    s_have = true;
}

nema_err_t pal_power_apply(int profile) {
    if (profile < 0 || profile > 2) return NEMA_ERR_INVAL;
    s_profile = profile;
    if (!s_have || !P.cpu || P.max_mhz <= 0) return s_last = NEMA_ERR_NOSYS;
    int top = P.max_mhz;
    /* performance: the top clock all the time; balanced: down to 80 MHz when idle; saver: half the top clock at most, 40 MHz idle */
    s_max = profile == PAL_POWER_SAVER ? top / 2 : top;
    s_min = profile == PAL_POWER_PERFORMANCE ? top : profile == PAL_POWER_BALANCED ? 80 : 40;
    if (s_min > s_max) s_min = s_max;
    return s_last = P.cpu(s_min, s_max);
}

int pal_power_profile(void) { return s_profile; }

void pal_power_describe(char *out, size_t cap) {
    if (s_last == NEMA_OK && s_min == s_max) snprintf(out, cap, "%s: CPU %d MHz", NAMES[s_profile], s_max);
    else if (s_last == NEMA_OK) snprintf(out, cap, "%s: CPU %d to %d MHz", NAMES[s_profile], s_min, s_max);
    else if (s_last == NEMA_ERR_NOSYS) snprintf(out, cap, "%s (this board keeps its clock)", NAMES[s_profile]);
    else snprintf(out, cap, "%s (not applied: %d)", NAMES[s_profile], (int)s_last);
}
