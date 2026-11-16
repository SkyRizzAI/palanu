/* `app`: list, inspect, install, run, and remove apps. The list is the registry: built-in commands and views,
 * and installed packages, in one table. Installed apps also run by their id, like any command. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/papp.h"
#include "pal/registry.h"

static const char *kind_name(pal_kind_t k) { return k == PAL_KIND_SERVICE ? "service" : k == PAL_KIND_GUI ? "gui" : "tty"; }

/* The permission names of a mask, in the order docs/PAPP.md lists them. */
static void caps_text(uint32_t c, char *out, size_t cap) {
    static const struct {
        uint32_t bit;
        const char *name;
    } N[] = {
        {PAL_PERM_FS, "fs"},
        {PAL_PERM_FS_HOME, "fs.home"},
        {PAL_PERM_FS_MEDIA, "fs.media"},
        {PAL_PERM_NET, "net"},
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
    out[0] = 0;
    for (size_t i = 0; i < sizeof(N) / sizeof(N[0]); i++) {
        if (!(c & N[i].bit)) continue;
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s", used ? "," : "", N[i].name);
    }
    if (!out[0]) snprintf(out, cap, "-");
}

static void print_info(const pal_papp_t *a) {
    char caps[96];
    caps_text(a->perms, caps, sizeof(caps));
    pal_printf("id        %s\nname      %s\nversion   %s\nformat    %u\napi       %u\nkind      %s%s\n"
               "caps      %s\nmemory    %lu KB\nstack     %lu KB\ncode      %lu bytes\n",
               a->id, a->name, a->version, (unsigned)a->format, a->api, kind_name(a->kind), a->hidden ? " (hidden)" : "",
               caps, (unsigned long)(a->mem_kb ? a->mem_kb : 256), (unsigned long)(a->stack_kb ? a->stack_kb : 16),
               (unsigned long)a->payload_len);
}

static int cmd_list(void) {
    pal_printf("%-14s %-8s %-8s %-9s %-7s %s\n", "ID", "VERSION", "KIND", "SOURCE", "MEMORY", "CAPS");
    size_t n = psys_reg_count();
    for (size_t i = 0; i < n; i++) {
        pal_reg_app_t e;
        if (!psys_reg_get(i, &e)) continue;
        char caps[96], mem[12];
        caps_text(e.caps, caps, sizeof(caps));
        if (e.mem_kb) snprintf(mem, sizeof(mem), "%luK", (unsigned long)e.mem_kb);
        else snprintf(mem, sizeof(mem), "-");
        pal_printf("%-14s %-8s %-8s %-9s %-7s %s%s\n", e.id, e.version[0] ? e.version : "-", kind_name(e.kind),
                   e.source == PAL_SRC_INSTALLED ? "installed" : "builtin", mem, caps, e.hidden ? "  (hidden)" : "");
    }
    if (n == 0) pal_printf("no apps\n");
    return 0;
}

static int cmd_app(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "list";
    if (strcmp(sub, "list") == 0) return cmd_list();
    if (argc < 3) {
        pal_eprintf("usage: app [list | info FILE | install FILE | remove ID | run FILE [args]]\n");
        return 2;
    }
    if (strcmp(sub, "info") == 0) {
        uint8_t *buf;
        pal_papp_t a;
        nema_err_t r = pal_papp_load(argv[2], &buf, &a);
        if (r != NEMA_OK) {
            pal_eprintf("app: %s: %s\n", argv[2], pal_papp_strerror(r));
            return 1;
        }
        print_info(&a);
        nema_free(buf);
        return 0;
    }
    if (strcmp(sub, "run") == 0) return pal_papp_run_file(argv[2], argc - 2, argv + 2);
    if (strcmp(sub, "install") == 0) {
        char id[24], msg[120];
        nema_err_t r = pal_papp_install_msg(argv[2], id, sizeof(id), msg, sizeof(msg));
        if (r != NEMA_OK) {
            pal_eprintf("app: install failed: %s\n", msg[0] ? msg : pal_papp_strerror(r));
            return 1;
        }
        pal_printf("installed '%s': run it by typing %s\n", id, id);
        return 0;
    }
    if (strcmp(sub, "remove") == 0) {
        nema_err_t r = pal_papp_remove(argv[2]);
        if (r != NEMA_OK) {
            pal_eprintf("app: %s: %s\n", argv[2], r == NEMA_ERR_NOENT ? "not installed" : pal_strerror(r));
            return 1;
        }
        pal_printf("removed %s\n", argv[2]);
        return 0;
    }
    pal_eprintf("app: unknown subcommand '%s'\n", sub);
    return 2;
}

void pal_apps_register_app(void) {
    static const nema_app_t APP = {"app", "list, install, run, and remove apps", cmd_app, NEMA_APP_STACK_DEFAULT * 2, 0};
    nema_app_register(&APP);
}
