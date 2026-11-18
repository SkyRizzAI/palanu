/* The registry of apps (system/include/pal/registry.h). Entries are the built-in views (a node each, copied from the
 * generated table), installed packages (a node each, heap, owner "registry"), and the commands of the nema_app_t table
 * (not copied: read from nema and given the command defaults when listed). A command with the same id as a node is
 * shadowed by it: a package can never shadow a built-in command (papp_pkg.c refuses that name). */
#include "pal/registry.h"

#include <stdio.h>
#include <string.h>

#include "nema/proc.h"

typedef struct reg_node {
    pal_reg_app_t app;
    struct reg_node *next;
} reg_node_t;

static struct {
    reg_node_t *head, *tail;
    size_t nodes;
    const pal_reg_defaults_t *defaults;
    nema_owner_t owner;
    bool ready;
} R = {.defaults = NULL};

static nema_owner_t reg_owner(void) {
    if (!R.owner) R.owner = nema_owner_register("registry");
    return R.owner;
}

static reg_node_t *node_find(const char *id) {
    for (reg_node_t *n = R.head; n; n = n->next)
        if (strcmp(n->app.id, id) == 0) return n;
    return NULL;
}

static bool shadowed(const char *id) { return node_find(id) != NULL; }

static void node_add(const pal_reg_app_t *e) {
    reg_node_t *n = (reg_node_t *)nema_alloc(NEMA_MEM_ANY, reg_owner(), sizeof(*n));
    if (!n) return;
    n->app = *e;
    n->next = NULL;
    if (R.tail) R.tail->next = n;
    else R.head = n;
    R.tail = n;
    R.nodes++;
}

static void node_del(const char *id) {
    reg_node_t *prev = NULL;
    for (reg_node_t *n = R.head; n; prev = n, n = n->next) {
        if (strcmp(n->app.id, id) != 0) continue;
        if (prev) prev->next = n->next;
        else R.head = n->next;
        if (R.tail == n) R.tail = prev;
        nema_free(n);
        R.nodes--;
        return;
    }
}

/* A command of the nema table: id is its name, the defaults give the rest. */
static void command_entry(const nema_app_t *a, pal_reg_app_t *e) {
    memset(e, 0, sizeof(*e));
    nema_strlcpy(e->id, a->name, sizeof(e->id));
    nema_strlcpy(e->name, a->help ? a->help : a->name, sizeof(e->name));
    e->api = R.defaults ? R.defaults->api : 2;
    e->kind = R.defaults ? R.defaults->kind : PAL_KIND_TTY;
    e->hidden = R.defaults ? R.defaults->hidden : true;
    e->source = PAL_SRC_BUILTIN;
    e->stack_kb = (uint32_t)(a->stack / 1024u);
}

static void builtin_entry(const pal_reg_builtin_t *b, pal_reg_app_t *e) {
    memset(e, 0, sizeof(*e));
    nema_strlcpy(e->id, b->id, sizeof(e->id));
    nema_strlcpy(e->name, b->name ? b->name : b->id, sizeof(e->name));
    nema_strlcpy(e->version, b->version ? b->version : "", sizeof(e->version));
    nema_strlcpy(e->icon, b->icon ? b->icon : "", sizeof(e->icon));
    nema_strlcpy(e->category, b->category ? b->category : "", sizeof(e->category));
    nema_strlcpy(e->desktops, b->desktops ? b->desktops : "", sizeof(e->desktops));
    e->api = b->api;
    e->kind = b->kind;
    e->hidden = b->hidden;
    e->caps = b->caps;
    e->stack_kb = b->stack_kb;
    e->mem_kb = b->mem_kb;
    e->fuel_m = b->fuel_m;
    e->source = PAL_SRC_BUILTIN;
}

void psys_reg_init(const pal_reg_builtin_t *tab, size_t n, const pal_reg_defaults_t *cmd_defaults) {
    if (R.ready) return;
    R.ready = true;
    R.defaults = cmd_defaults;
    for (size_t i = 0; tab && i < n; i++) {
        pal_reg_app_t e;
        builtin_entry(&tab[i], &e);
        if (!node_find(e.id)) node_add(&e);
    }
}

size_t psys_reg_count(void) {
    size_t n = R.nodes;
    size_t c = nema_app_count();
    for (size_t i = 0; i < c; i++) {
        const nema_app_t *a = nema_app_at(i);
        if (a && !shadowed(a->name)) n++;
    }
    return n;
}

bool psys_reg_get(size_t i, pal_reg_app_t *out) {
    if (i < R.nodes) {
        reg_node_t *n = R.head;
        for (size_t k = 0; n && k < i; k++) n = n->next;
        if (!n) return false;
        *out = n->app;
        return true;
    }
    size_t want = i - R.nodes, c = nema_app_count();
    for (size_t k = 0; k < c; k++) {
        const nema_app_t *a = nema_app_at(k);
        if (!a || shadowed(a->name)) continue;
        if (want-- == 0) {
            command_entry(a, out);
            return true;
        }
    }
    return false;
}

bool psys_reg_find(const char *id, pal_reg_app_t *out) {
    reg_node_t *n = node_find(id);
    if (n) {
        *out = n->app;
        return true;
    }
    const nema_app_t *a = nema_app_find(id);
    if (!a) return false;
    command_entry(a, out);
    return true;
}

nema_err_t psys_reg_add_installed(const pal_papp_t *app, const char *path) {
    if (node_find(app->id)) return NEMA_ERR_EXIST;
    const nema_app_t *a = nema_app_find(app->id);
    if (a) return NEMA_ERR_EXIST; /* a built-in command keeps its name */
    pal_reg_app_t e;
    memset(&e, 0, sizeof(e));
    nema_strlcpy(e.id, app->id, sizeof(e.id));
    nema_strlcpy(e.name, app->name[0] ? app->name : app->id, sizeof(e.name));
    nema_strlcpy(e.version, app->version, sizeof(e.version));
    nema_strlcpy(e.icon, app->icon, sizeof(e.icon));
    nema_strlcpy(e.category, app->category, sizeof(e.category));
    nema_strlcpy(e.desktops, app->desktops, sizeof(e.desktops));
    nema_strlcpy(e.path, path ? path : "", sizeof(e.path));
    e.api = app->api;
    e.kind = app->kind;
    e.hidden = app->hidden;
    e.source = PAL_SRC_INSTALLED;
    e.caps = app->perms;
    e.stack_kb = app->stack_kb;
    e.mem_kb = app->mem_kb;
    e.fuel_m = app->fuel_m;
    size_t before = R.nodes;
    node_add(&e);
    return R.nodes > before ? NEMA_OK : NEMA_ERR_NOMEM;
}

nema_err_t psys_reg_remove(const char *id) {
    reg_node_t *n = node_find(id);
    if (!n || n->app.source != PAL_SRC_INSTALLED) return NEMA_ERR_NOENT;
    node_del(id);
    return NEMA_OK;
}

nema_err_t psys_reg_check_limits(pal_kind_t kind, size_t running_wasm, size_t running_services) {
    if (running_wasm >= PSYS_REG_MAX_WASM) return NEMA_ERR_FULL;
    if (kind == PAL_KIND_SERVICE && running_services >= PSYS_REG_MAX_SERVICES) return NEMA_ERR_FULL;
    return NEMA_OK;
}

nema_err_t psys_reg_admit(const pal_reg_app_t *app, int32_t self, char *msg, size_t cap) {
    /* Every live process is looked at under the process table's lock by nema_proc_list. The records are copied into one
     * block from the heap: a stack array of them would not fit an app's task. */
    size_t n = NEMA_PROC_MAX;
    nema_proc_t *procs = (nema_proc_t *)nema_alloc(NEMA_MEM_ANY, reg_owner(), n * sizeof(*procs));
    if (!procs) {
        if (msg && cap) snprintf(msg, cap, "%s: out of memory", app->id);
        return NEMA_ERR_NOMEM; /* an unknown count is never a yes */
    }
    size_t wasm = 0, services = 0;
    n = nema_proc_list(procs, n);
    for (size_t i = 0; i < n; i++) {
        const nema_proc_t *p = &procs[i];
        if (p->state != NEMA_PROC_RUNNING || p->pid == self || !p->app) continue;
        reg_node_t *node = node_find(p->app->name);
        if (!node || node->app.source != PAL_SRC_INSTALLED) continue;
        wasm++;
        if (node->app.kind == PAL_KIND_SERVICE) services++;
    }
    nema_free(procs);
    nema_err_t r = psys_reg_check_limits(app->kind, wasm, services);
    if (r != NEMA_OK && msg && cap) {
        if (wasm >= PSYS_REG_MAX_WASM) snprintf(msg, cap, "%s: %d apps are already running (limit %d); stop one first", app->id, (int)wasm, PSYS_REG_MAX_WASM);
        else snprintf(msg, cap, "%s: %d services are already running (limit %d)", app->id, (int)services, PSYS_REG_MAX_SERVICES);
    }
    return r;
}

bool psys_reg_in_desktop(const pal_reg_app_t *e, const char *desktop) {
    if (!e->desktops[0]) return true;
    if (!desktop || !desktop[0]) return false;
    size_t n = strlen(desktop);
    for (const char *p = e->desktops; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !strncmp(p, desktop, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
