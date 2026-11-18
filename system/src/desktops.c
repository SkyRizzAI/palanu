/* The desktop registry (pal/desktops.h). Registration happens at start-up from one task; use and boot run from the shell or
 * the boot task, so a mutex is not needed: the compositor takes its own lock for every change this makes to it. */
#include <string.h>

#include "nema/log.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/settings.h"

static const pal_desktop_env_t *s_env[PAL_DESKTOP_ENV_MAX];
static size_t s_n;
static const pal_desktop_env_t *s_active;

/* The fallback is the desktop called "classic" (registered by ui_wire.c); a platform may register others before it. */
static const pal_desktop_env_t *fallback(void) {
    const pal_desktop_env_t *c = pal_desktops_find("classic");
    return c ? c : (s_n ? s_env[0] : NULL);
}

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

static bool start_env(const pal_desktop_env_t *e) {
    if (s_active && s_active->stop) s_active->stop();
    s_active = NULL;
    if (!e->start()) return false;
    s_active = e;
    return true;
}

nema_err_t pal_desktops_use(const char *id) {
    const pal_desktop_env_t *e = pal_desktops_find(id);
    if (!e) return NEMA_ERR_NOENT;
    if (!start_env(e)) {
        const pal_desktop_env_t *fb = fallback();
        NEMA_LOGW("desktop", "%s did not start; back to %s", e->id, fb->id);
        if (e != fb) start_env(fb);
        pal_desktop_home();
        return NEMA_ERR_IO;
    }
    pal_desktop_home();
    psys_settings_set("desktop.env", e->id);
    return NEMA_OK;
}

void pal_desktops_boot(void) {
    const pal_desktop_env_t *fb = fallback();
    if (!fb) return;
    char want[24] = "";
    psys_settings_get("desktop.env", want, sizeof(want));
    const pal_desktop_env_t *e = pal_desktops_find(want);
    if (!e) e = fb;
    if (!start_env(e) && e != fb) start_env(fb);
}
