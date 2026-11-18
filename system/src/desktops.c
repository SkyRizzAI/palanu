/* The desktop registry (pal/desktops.h). Registration happens at start-up from one task; use and boot run from the shell or
 * the boot task, so a mutex is not needed: the compositor takes its own lock for every change this makes to it. There is no
 * fallback desktop: what is under every session is the system console (ADR-0010). */
#include <string.h>

#include "nema/log.h"
#include "pal/bootcount.h"
#include "pal/console.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/settings.h"

static const pal_desktop_env_t *s_env[PAL_DESKTOP_ENV_MAX];
static size_t s_n;
static const pal_desktop_env_t *s_active;

nema_err_t pal_desktops_register(const pal_desktop_env_t *env) {
    if (!env || !env->id || !env->id[0] || !env->start) return NEMA_ERR_INVAL;
    for (size_t i = 0; i < s_n; i++)
        if (!strcmp(s_env[i]->id, env->id)) return NEMA_ERR_EXIST;
    if (s_n == PAL_DESKTOP_ENV_MAX) return NEMA_ERR_FULL;
    s_env[s_n++] = env;
    return NEMA_OK;
}

size_t pal_desktops_count(void) { return s_n; }
const pal_desktop_env_t *pal_desktops_get(size_t index) { return index < s_n ? s_env[index] : NULL; }

const pal_desktop_env_t *pal_desktops_find(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < s_n; i++)
        if (!strcmp(s_env[i]->id, id)) return s_env[i];
    return NULL;
}

const char *pal_desktops_active(void) { return s_active ? s_active->id : ""; }

static void stop_active(void) {
    if (s_active && s_active->stop) s_active->stop();
    s_active = NULL;
}

/* No session: the reason goes to the log and the console, and the console takes the screen. */
static void failed(const char *id, const char *why) {
    NEMA_LOGE("desktop", "%s: %s", id, why);
    pal_console_printf("[\x1b[31mFAILED\x1b[0m] Desktop session %s: %s\n", id, why);
    pal_console_show();
}

nema_err_t pal_desktops_use(const char *id) {
    const pal_desktop_env_t *e = pal_desktops_find(id);
    if (!e) return NEMA_ERR_NOENT;
    stop_active();
    if (!e->start()) {
        failed(e->id, "did not start");
        return NEMA_ERR_IO;
    }
    s_active = e;
    pal_desktop_home();
    psys_settings_set("desktop.env", e->id);
    return NEMA_OK;
}

void pal_desktops_stop(void) {
    stop_active();
    pal_console_show();
}

void pal_desktops_boot(void) {
    if (psys_bootcount_safe()) { /* safe mode starts as little as it can; desktop.env is kept for the next normal boot */
        pal_console_print("Safe mode: no desktop session. `safemode off` and `reboot` leave it.\n");
        return;
    }
    char target[16] = "";
    psys_settings_get("boot.target", target, sizeof(target));
    if (!strcmp(target, "console")) return;
    char want[24] = "";
    psys_settings_get("desktop.env", want, sizeof(want));
    const pal_desktop_env_t *e = pal_desktops_find(want);
    if (!e) {
        failed(want[0] ? want : "(none)", "no such desktop; `desktop` lists them");
        return;
    }
    stop_active();
    if (!e->start()) {
        failed(e->id, "did not start");
        return;
    }
    s_active = e;
}
