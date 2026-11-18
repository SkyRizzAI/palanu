/* The registry of apps: one list that merges the built-in views (gui, desktop), the built-in commands (tty, the
 * nema_app_t table), and installed .papp packages. The launcher, `app list`, and admission all read it.
 *
 * Built-in views come from apps/apps.toml, compiled into apps/manifests.gen.c by forge/packages/appgen; no TOML is parsed
 * on the device. Commands take the [defaults.command] row. Installed apps are added by papp_pkg.c when they verify. */
#ifndef PAL_REGISTRY_H
#define PAL_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "pal/papp.h"

#define PSYS_REG_MAX_WASM 4     /* installed apps running at once (WebAssembly instances) */
#define PSYS_REG_MAX_SERVICES 6 /* service apps running at once */
#define PSYS_REG_MAX_GUI_FOCUS 1 /* gui apps in front at once */

typedef enum { PAL_SRC_BUILTIN = 0, PAL_SRC_INSTALLED = 1 } pal_src_t;

typedef struct {
    char id[24];
    char name[32];
    char version[16];
    uint16_t api;
    pal_kind_t kind;
    pal_src_t source;
    bool hidden;      /* not listed in the launcher */
    uint32_t caps;    /* PAL_PERM_* bits the app holds */
    uint32_t stack_kb;
    uint32_t mem_kb;  /* 0 = not limited by the manifest (commands) */
    uint32_t fuel_m;
    char icon[24];    /* icon id; empty means the id */
    char category[16];
    char path[48];    /* installed apps: where the package file is; empty for built-ins */
    char desktops[32]; /* the desktop sessions it belongs to, comma-separated; empty = every session (ADR-0010) */
} pal_reg_app_t;

/* One row of apps/manifests.gen.c (the generated built-in table). */
typedef struct {
    const char *id;
    const char *name;
    const char *version;
    uint16_t api;
    pal_kind_t kind;
    bool hidden;
    uint32_t caps;
    uint32_t stack_kb;
    uint32_t mem_kb;
    uint32_t fuel_m;
    const char *icon;
    const char *category;
    const char *desktops; /* "" = every session */
} pal_reg_builtin_t;

/* The defaults for built-in commands (the nema_app_t table): [defaults.command] in apps/apps.toml. */
typedef struct {
    pal_kind_t kind;
    bool hidden;
    uint16_t api;
} pal_reg_defaults_t;

/* The generated built-in table (apps/manifests.gen.c, from apps/apps.toml). */
extern const pal_reg_builtin_t PAL_BUILTIN_APPS[];
extern const size_t PAL_BUILTIN_APPS_COUNT;
extern const pal_reg_defaults_t PAL_BUILTIN_COMMAND_DEFAULTS;

/* Boot: the built-in views (in table order) and the command defaults. Idempotent. */
void psys_reg_init(const pal_reg_builtin_t *tab, size_t n, const pal_reg_defaults_t *cmd_defaults);

/* Listing: views and installed apps first (in order of addition), then the commands that are not shadowed by an entry
 * of the same id. psys_reg_get copies entry i into *out; false when i is out of range. */
size_t psys_reg_count(void);
bool psys_reg_get(size_t i, pal_reg_app_t *out);
bool psys_reg_find(const char *id, pal_reg_app_t *out);
/* The entry belongs to the session `desktop` (its desktops list is empty or names it). */
bool psys_reg_in_desktop(const pal_reg_app_t *e, const char *desktop);

/* Installed apps: added when a package verifies and its command is registered; removed with the command. */
nema_err_t psys_reg_add_installed(const pal_papp_t *app, const char *path);
nema_err_t psys_reg_remove(const char *id);

/* Admission: may one more app of this entry start? Counts the installed apps running now (any process whose
 * command is an installed entry), excluding process `self` (its own start). NEMA_ERR_FULL with msg set when a limit is
 * reached: PSYS_REG_MAX_WASM for any installed app, PSYS_REG_MAX_SERVICES for services. */
nema_err_t psys_reg_admit(const pal_reg_app_t *app, int32_t self, char *msg, size_t cap);
/* The limit rule alone, for counts the caller already has. NEMA_OK or NEMA_ERR_FULL. */
nema_err_t psys_reg_check_limits(pal_kind_t kind, size_t running_wasm, size_t running_services);

/* Launcher (system/src/launcher.c): the entries it lists are the ones with !hidden and kind tty or gui. */
size_t psys_launcher_count(void);
bool psys_launcher_get(size_t i, pal_reg_app_t *out);
/* The terminal runner: apps/boot.c gives pal_terminal_run, which opens the Terminal and types the command. */
void pal_launcher_set_runner(void (*run_tty)(const char *id));

#endif
