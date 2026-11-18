/* The desktop registry (pal/desktops.h). Registration happens at start-up from one task. Starting and stopping a session run on
 * the screen loop's task (a call from the shell waits for it there), because they add and take away views and overlays.
 * What a session registers during its start is its own; its stop takes all of it away (desk_session_reset). There is no
 * fallback desktop: what is under every session is the system console (ADR-0010). */
#include <string.h>

#include "desktop_int.h"
#include "nema/log.h"
#include "nema/nema.h"
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
    if (!s_active) return;
    if (s_active->stop) s_active->stop();
    desk_session_reset(s_active->id);
    s_active = NULL;
}

/* The session's start, with what it registers marked as its own; a failed start leaves nothing behind. */
static bool start_session(const pal_desktop_env_t *e) {
    desk_session_scope(e->id);
    bool ok = e->start();
    desk_session_scope(NULL);
    if (!ok) desk_session_reset(e->id);
    else s_active = e;
    return ok;
}

/* Runs fn on the screen loop's task and waits for it (or runs it here, on that task or before it starts). */
typedef struct {
    const char *id;
    int op; /* 0 use, 1 stop, 2 boot */
    nema_err_t r;
} job_t;

static void run_job(job_t *j);
static nema_port_sem_t *s_done;

static void post_job(void *arg) {
    run_job((job_t *)arg);
    nema_port_sem_give(s_done);
}

static nema_err_t on_loop(job_t *j) {
    if (!desk_started() || desk_on_task()) {
        run_job(j);
        return j->r;
    }
    if (!s_done) s_done = nema_port_sem_create(0, 1);
    if (!s_done || desk_post(post_job, j) != NEMA_OK) return NEMA_ERR_BUSY;
    if (nema_port_sem_take(s_done, 15000) != NEMA_OK) return NEMA_ERR_TIMEOUT;
    return j->r;
}

/* No session: the reason goes to the log and the console, and the console takes the screen. */
static void failed(const char *id, const char *why) {
    NEMA_LOGE("desktop", "%s: %s", id, why);
    pal_console_printf("[\x1b[31mFAILED\x1b[0m] Desktop session %s: %s\n", id, why);
    pal_console_show();
}

static nema_err_t use_now(const char *id) {
    const pal_desktop_env_t *e = pal_desktops_find(id);
    if (!e) return NEMA_ERR_NOENT;
    stop_active();
    if (!start_session(e)) {
        failed(e->id, "did not start");
        return NEMA_ERR_IO;
    }
    pal_desktop_home();
    psys_settings_set("desktop.env", e->id);
    return NEMA_OK;
}

static void boot_now(void) {
    if (psys_bootcount_safe()) { /* safe mode starts as little as it can; desktop.env is kept for the next normal boot */
        pal_console_print("Safe mode: no desktop session. `safemode off` and `reboot` leave it.\n");
        return;
    }
    char target[16] = "";
    psys_settings_get("boot.target", target, sizeof(target));
    if (!strcmp(target, "console")) return;
    if (s_n == 0) { /* this firmware ships no desktop: the system console is the whole user interface */
        pal_console_print("No desktop session in this build: the console is the user interface.\n");
        return;
    }
    char want[24] = "";
    psys_settings_get("desktop.env", want, sizeof(want));
    const pal_desktop_env_t *e = want[0] ? pal_desktops_find(want) : s_env[0]; /* unset: the first the target lists */
    if (!e) {
        failed(want[0] ? want : "(none)", "no such desktop; `desktop` lists them");
        return;
    }
    stop_active();
    if (!start_session(e)) failed(e->id, "did not start");
}

static void run_job(job_t *j) {
    j->r = NEMA_OK;
    if (j->op == 0) j->r = use_now(j->id);
    else if (j->op == 1) {
        stop_active();
        pal_console_show();
    } else boot_now();
}

nema_err_t pal_desktops_use(const char *id) {
    job_t j = {id, 0, NEMA_OK};
    return on_loop(&j);
}

void pal_desktops_stop(void) {
    job_t j = {NULL, 1, NEMA_OK};
    (void)on_loop(&j);
}

void pal_desktops_boot(void) {
    job_t j = {NULL, 2, NEMA_OK};
    (void)on_loop(&j);
}
