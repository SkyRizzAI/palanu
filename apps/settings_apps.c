/* Settings pages: Apps (the registry: built-in views, services, installed packages), the app detail (identity, memory, the
 * permissions the manifest asks for, storage, Run, Uninstall, and for services Start, Stop, start at boot and the log), and
 * the services page (state, restart). The registry is read with psys_reg_*; the services with psys_svc_*. Uninstall and the
 * service starts go through the job worker. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "nema/vfs.h"
#include "pal/desktop.h"
#include "pal/papp.h"
#include "pal/registry.h"
#include "pal/svc.h"
#include "settings_int.h"

#define APP_SLOTS 32
#define SVC_SLOTS PSYS_SVC_MAX
#define LOG_LINES 10

/* What the list rows point at: copies of the registry entries shown (labels and ids must stay valid while the page is up). */
static struct {
    char id[24], name[32], icon[24];
    bool installed;
    pal_kind_t kind;
} s_apps[APP_SLOTS] NEMA_EXT_BSS;
static size_t s_napps;
static size_t s_hidden_left;

static char s_detail_id[24];   /* the app the detail page shows */
static char s_svc_id[24];      /* the service the service pages show */

static pal_menu_row_t APPS_STORE[APP_SLOTS + 2] NEMA_EXT_BSS;
static pal_menu_row_t DETAIL_STORE[32] NEMA_EXT_BSS;
static pal_menu_row_t SERVICES_STORE[SVC_SLOTS + 2] NEMA_EXT_BSS;
static pal_menu_row_t SVC_DETAIL_STORE[16] NEMA_EXT_BSS;
static pal_menu_row_t LOG_STORE[LOG_LINES + 2] NEMA_EXT_BSS;
static char s_log_tail[1024] NEMA_EXT_BSS;
static char s_svc_names[SVC_SLOTS][40] NEMA_EXT_BSS;
static char s_svc_ids[SVC_SLOTS][PSYS_SVC_ID_LEN];

static const char *kind_name(pal_kind_t k) {
    switch (k) {
    case PAL_KIND_SERVICE:
        return "service";
    case PAL_KIND_TTY:
        return "terminal app";
    default:
        return "app";
    }
}

/* Shown in the list: every view, every service and every installed package. Commands stay out (the Terminal has them). */
static bool listed(const pal_reg_app_t *a) { return !a->hidden || a->source == PAL_SRC_INSTALLED || a->kind == PAL_KIND_SERVICE; }

static void open_app_detail_ctx(const char *id) { snprintf(s_detail_id, sizeof(s_detail_id), "%s", id); }

static pal_menu_page_t *open_app(void *ctx) {
    open_app_detail_ctx(((const char *)ctx));
    return &PG_APP_DETAIL;
}

static void build_apps(pal_menu_page_t *p) {
    pal_menu_clear(p);
    s_napps = 0;
    s_hidden_left = 0;
    size_t n = psys_reg_count();
    for (size_t i = 0; i < n; i++) {
        pal_reg_app_t a;
        if (!psys_reg_get(i, &a) || !listed(&a)) continue;
        if (s_napps >= APP_SLOTS) {
            s_hidden_left++;
            continue;
        }
        size_t k = s_napps++;
        snprintf(s_apps[k].id, sizeof(s_apps[k].id), "%s", a.id);
        snprintf(s_apps[k].name, sizeof(s_apps[k].name), "%s", a.name);
        snprintf(s_apps[k].icon, sizeof(s_apps[k].icon), "%s", a.icon[0] ? a.icon : a.id);
        s_apps[k].installed = a.source == PAL_SRC_INSTALLED;
        s_apps[k].kind = a.kind;
        pal_menu_row_t r = {.kind = PAL_MENU_SUBMENU, .label = s_apps[k].name, .icon = s_apps[k].icon,
                            .ctx = s_apps[k].id, .open = open_app, .hint = s_apps[k].installed ? "Installed package" : NULL};
        pal_menu_add(p, &r);
    }
    if (s_napps == 0) {
        pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "No apps", .text = settings_text_static, .ctx = (void *)"Nothing listed"};
        pal_menu_add(p, &r);
    }
    if (s_hidden_left) {
        static char more[24];
        snprintf(more, sizeof(more), "%u more", (unsigned)s_hidden_left);
        pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "Not shown", .text = settings_text_static, .ctx = more};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_APPS = {.title = "Apps", .store = APPS_STORE, .cap = APP_SLOTS + 2, .build = build_apps};

/* ---- the app detail ---- */

static const struct {
    uint32_t bit;
    const char *name;
} PERMS[] = {
    {PAL_PERM_FS, "fs"},
    {PAL_PERM_NET, "net"},
    {PAL_PERM_FS_HOME, "fs.home"},
    {PAL_PERM_FS_MEDIA, "fs.media"},
    {PAL_PERM_NET_LISTEN, "net.listen"},
    {PAL_PERM_PROC, "proc.spawn"},
    {PAL_PERM_CRYPTO_KEYS, "crypto.keys"},
    {PAL_PERM_SETTINGS_READ, "settings.read"},
    {PAL_PERM_SETTINGS_WRITE, "settings.write"},
    {PAL_PERM_NOTIFY, "notify"},
    {PAL_PERM_SVC_CONTROL, "svc.control"},
    {PAL_PERM_LINK, "link"},
    {PAL_PERM_DEV, "dev"},
};

static void text_of(void *ctx, char *out, size_t cap) { snprintf(out, cap, "%s", (const char *)ctx); }

static void run_app(void *ctx) {
    (void)ctx;
    pal_reg_app_t a;
    if (!psys_reg_find(s_detail_id, &a)) return;
    if (a.kind == PAL_KIND_GUI) pal_desktop_open(a.id);
    else if (a.kind == PAL_KIND_TTY) pal_terminal_run(a.id);
    else settings_toast("Services start from the Start row");
}

static void open_logs_run(void *ctx) {
    (void)ctx;
    snprintf(s_svc_id, sizeof(s_svc_id), "%s", s_detail_id);
    pal_menu_push(settings_menu(), &PG_SVC_LOG);
}

static void uninstall_run(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_UNINSTALL, s_detail_id, NULL, "Removing app...");
}

static void kind_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_reg_app_t a;
    snprintf(out, cap, "%s", psys_reg_find(s_detail_id, &a) ? kind_name(a.kind) : "gone");
}

static void version_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_reg_app_t a;
    snprintf(out, cap, "%s", psys_reg_find(s_detail_id, &a) && a.version[0] ? a.version : "-");
}

static void source_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_reg_app_t a;
    snprintf(out, cap, "%s", psys_reg_find(s_detail_id, &a) && a.source == PAL_SRC_INSTALLED ? "installed" : "built-in");
}

static void limits_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_reg_app_t a;
    if (!psys_reg_find(s_detail_id, &a)) {
        snprintf(out, cap, "-");
        return;
    }
    if (!a.stack_kb && !a.mem_kb) snprintf(out, cap, "built-in default");
    else snprintf(out, cap, "stack %u KB, memory %u KB", (unsigned)a.stack_kb, (unsigned)a.mem_kb);
}

static void id_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", s_detail_id);
}

/* The app's storage, as the reference Used row: the package size on the internal flash ("0 B / Internal" for a built-in app, which
 * has no package in the flash). */
static void used_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_reg_app_t a;
    if (!psys_reg_find(s_detail_id, &a) || a.source != PAL_SRC_INSTALLED) {
        snprintf(out, cap, "0 B / Internal");
        return;
    }
    nema_stat_t st;
    char path[64];
    snprintf(path, sizeof(path), "/apps/%s/app.papp", s_detail_id);
    if (nema_fs_stat(path, &st) != NEMA_OK) snprintf(out, cap, "unknown");
    else if (st.size >= 1024u) snprintf(out, cap, "%u KB / Internal", (unsigned)(st.size / 1024u));
    else snprintf(out, cap, "%u B / Internal", (unsigned)st.size);
}

/* Service actions on the app's own service (apps with kind service). */
static bool app_is_service(void) {
    pal_reg_app_t a;
    return psys_reg_find(s_detail_id, &a) && a.kind == PAL_KIND_SERVICE;
}

static void svc_start_run(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_SVC_START, s_detail_id, NULL, "Starting...");
}
static void svc_stop_run(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_SVC_STOP, s_detail_id, NULL, "Stopping...");
}

static int svc_enabled_get(void *ctx) {
    (void)ctx;
    psys_svc_info_t in;
    return psys_svc_find(s_detail_id, &in) && in.enabled;
}
static void svc_enabled_set(void *ctx, int v) {
    (void)ctx;
    (void)psys_svc_set_enabled(s_detail_id, v != 0);
}

static void perm_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "granted");
}

/* The reference app detail: the app's name, Permissions, Storage, Manage (Uninstall). The PalanuOS facts
 * and the run and service controls follow under System. A built-in app has no package to uninstall, so its Uninstall is N/A. */
static void build_detail(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_reg_app_t a;
    bool found = psys_reg_find(s_detail_id, &a);
    static char name[40];
    snprintf(name, sizeof(name), "%s", found ? a.name : s_detail_id);
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .inert = true, .label = name};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Permissions"};
    pal_menu_add(p, &r);
    int shown = 0;
    if (found)
        for (size_t i = 0; i < sizeof(PERMS) / sizeof(PERMS[0]); i++)
            if (a.caps & PERMS[i].bit) {
                r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = PERMS[i].name, .text = perm_text};
                pal_menu_add(p, &r);
                shown++;
            }
    if (!shown) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No permissions requested"};
        pal_menu_add(p, &r);
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Storage"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Used", .text = used_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Manage"};
    pal_menu_add(p, &r);
    bool installed = found && a.source == PAL_SRC_INSTALLED;
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Uninstall", .icon = "lock", .danger = true, .run = uninstall_run,
                         .disabled = !installed, .confirm = "Removes the app and its files.", .ok_label = "Uninstall"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "System"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Name", .text = text_of, .ctx = name};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Id", .text = id_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Version", .text = version_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Kind", .text = kind_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Source", .text = source_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Memory", .text = limits_text};
    pal_menu_add(p, &r);
    if (app_is_service()) {
        psys_svc_info_t in;
        bool running = psys_svc_find(s_detail_id, &in) && in.state == PSYS_SVC_RUNNING;
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = running ? "Stop" : "Start", .icon = "services",
                             .run = running ? svc_stop_run : svc_start_run, .disabled = found && in.core && running,
                             .hint = "Runs in the background as a service"};
        pal_menu_add(p, &r);
        r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Start at boot", .get = svc_enabled_get,
                             .set = svc_enabled_set, .hint = "Off: stays stopped after a restart"};
        pal_menu_add(p, &r);
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Logs", .icon = "logs", .run = open_logs_run,
                             .hint = "The last lines of its log"};
        pal_menu_add(p, &r);
    } else {
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Run", .icon = "terminal", .run = run_app,
                             .hint = "Opens it in front"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_APP_DETAIL = {.title = "App", .store = DETAIL_STORE, .cap = 32, .build = build_detail};

/* The app the detail page shows (the route app-NAME, and the Apps list before it pushes the page). */
void settings_apps_select(const char *id) { snprintf(s_detail_id, sizeof(s_detail_id), "%s", id ? id : ""); }

/* ---- the services ---- */

static pal_menu_page_t *open_svc(void *ctx) {
    snprintf(s_svc_id, sizeof(s_svc_id), "%s", (const char *)ctx);
    return &PG_SERVICE_DETAIL;
}

static void svc_state_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    snprintf(out, cap, "%s", psys_svc_find((const char *)ctx, &in) ? psys_svc_state_name(in.state) : "gone");
}

static void build_services(pal_menu_page_t *p) {
    pal_menu_clear(p);
    size_t n = psys_svc_count();
    size_t shown = 0;
    for (size_t i = 0; i < n && shown < SVC_SLOTS; i++) {
        psys_svc_info_t in;
        if (!psys_svc_info(i, &in)) continue;
        snprintf(s_svc_ids[shown], sizeof(s_svc_ids[shown]), "%s", in.id);
        snprintf(s_svc_names[shown], sizeof(s_svc_names[shown]), "%s", in.name[0] ? in.name : in.id);
        pal_menu_row_t r = {.kind = PAL_MENU_SUBMENU, .label = s_svc_names[shown], .text = svc_state_text,
                            .ctx = s_svc_ids[shown], .open = open_svc,
                            .hint = in.core ? "Core: always running" : NULL};
        pal_menu_add(p, &r);
        shown++;
    }
    if (shown == 0) {
        pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "No services", .text = text_of, .ctx = (void *)"none in this build"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_SERVICES = {.title = "Services", .store = SERVICES_STORE, .cap = SVC_SLOTS + 2, .build = build_services,
                               .live = true};

/* ---- one service: state, restart, start at boot, the log ---- */

static void open_logs_app(void *ctx) {
    (void)ctx;
    pal_desktop_open("logs");
}

static void svc_restart_run(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_SVC_RESTART, s_svc_id, NULL, "Restarting...");
}

static void svc_run_start(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_SVC_START, s_svc_id, NULL, "Starting...");
}

static void svc_run_stop(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_SVC_STOP, s_svc_id, NULL, "Stopping...");
}

static void svc_state_long(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    if (!psys_svc_find(s_svc_id, &in)) {
        snprintf(out, cap, "gone");
        return;
    }
    if (in.reason[0]) snprintf(out, cap, "%s: %s", psys_svc_state_name(in.state), in.reason);
    else snprintf(out, cap, "%s", psys_svc_state_name(in.state));
}

static void svc_restarts_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    if (!psys_svc_find(s_svc_id, &in)) snprintf(out, cap, "-");
    else snprintf(out, cap, "%lu", (unsigned long)in.restarts);
}

static void svc_failures_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    if (!psys_svc_find(s_svc_id, &in)) snprintf(out, cap, "-");
    else snprintf(out, cap, "%lu in 10 min", (unsigned long)in.failures);
}

static void svc_source_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    if (!psys_svc_find(s_svc_id, &in)) {
        snprintf(out, cap, "-");
        return;
    }
    snprintf(out, cap, "%s%s", in.installed ? "installed" : "built-in", in.core ? ", core" : "");
}

static bool svc_running(void) {
    psys_svc_info_t in;
    return psys_svc_find(s_svc_id, &in) && in.state == PSYS_SVC_RUNNING;
}

static bool svc_core(void) {
    psys_svc_info_t in;
    return psys_svc_find(s_svc_id, &in) && in.core;
}

static void build_svc_detail(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "State", .text = svc_state_long};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Restarts", .text = svc_restarts_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Failures", .text = svc_failures_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Source", .text = svc_source_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Control"};
    pal_menu_add(p, &r);
    if (svc_running()) {
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Stop", .icon = "services", .run = svc_run_stop,
                             .disabled = svc_core(), .hint = svc_core() ? "Core services cannot be stopped" : NULL};
        pal_menu_add(p, &r);
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Restart", .icon = "services", .run = svc_restart_run};
        pal_menu_add(p, &r);
    } else {
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Start", .icon = "services", .run = svc_run_start};
        pal_menu_add(p, &r);
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Start at boot", .get = svc_enabled_get, .set = svc_enabled_set};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Log", .icon = "logs", .open = settings_open_ctx,
                         .ctx = &PG_SVC_LOG, .hint = "The last lines of its log"};
    pal_menu_add(p, &r);
    if (pal_desktop_view("logs")) {
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Open in Logs", .icon = "logs", .run = open_logs_app,
                             .hint = "The whole log, in the Logs app"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_SERVICE_DETAIL = {.title = "Service", .store = SVC_DETAIL_STORE, .cap = 16, .build = build_svc_detail,
                                     .live = true};

/* ---- the log tail: the last lines the service wrote (the Logs app, when it is there, shows all of it) ---- */

static void build_log(pal_menu_page_t *p) {
    pal_menu_clear(p);
    size_t tl = 0;
    uint64_t cursor = 0;
    char chunk[256];
    for (;;) {
        int n = psys_svc_log_read(s_svc_id, &cursor, chunk, sizeof(chunk));
        if (n <= 0) break;
        size_t take = (size_t)n; /* at most sizeof(chunk), which is smaller than the tail */
        _Static_assert(sizeof(chunk) < sizeof(s_log_tail), "a chunk fits in the tail");
        if (tl + take > sizeof(s_log_tail)) {
            size_t drop = tl + take - sizeof(s_log_tail);
            memmove(s_log_tail, s_log_tail + drop, tl - drop);
            tl -= drop;
        }
        memcpy(s_log_tail + tl, chunk, take);
        tl += take;
    }
    s_log_tail[tl < sizeof(s_log_tail) ? tl : sizeof(s_log_tail) - 1] = 0;
    /* the lines, newest last; the last LOG_LINES of them */
    char *starts[64];
    size_t ns = 0;
    char *p0 = s_log_tail;
    for (char *q = p0; *q; q++) {
        if (*q == '\n') {
            *q = 0;
            if (ns < 64) starts[ns++] = p0;
            p0 = q + 1;
        }
    }
    if (*p0 && ns < 64) starts[ns++] = p0;
    size_t from = ns > LOG_LINES ? ns - LOG_LINES : 0;
    for (size_t i = from; i < ns; i++) {
        pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = starts[i][0] ? starts[i] : " ", .wrap = false};
        pal_menu_add(p, &r);
    }
    if (ns == 0) {
        pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "Log is empty", .text = text_of, .ctx = (void *)"nothing written yet"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_SVC_LOG = {.title = "Log", .store = LOG_STORE, .cap = LOG_LINES + 2, .build = build_log};
