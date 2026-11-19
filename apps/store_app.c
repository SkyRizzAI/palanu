/* Store (view "store") and the `store` command: the app catalogue (pal/store.h). The view is a form (pal_menu_bind, ADR-0012), so
 * each desktop draws it in its own look: the catalogue's apps with their version and whether they are installed, and a page per
 * app with its summary and Install or Update. The network work runs on a worker task; the busy box shows while it runs. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/menu.h"
#include "pal/store.h"
#include "pal/theme.h"
#include "pal/toast.h"
#include "views.h"

/* ---- the worker ---- */

enum { JOB_NONE, JOB_REFRESH, JOB_INSTALL };
static struct {
    volatile int job, done_job;
    volatile bool busy, done;
    nema_err_t rc;
    char id[24], msg[96];
} W NEMA_EXT_BSS;

static void worker(void *arg) {
    (void)arg;
    if (W.job == JOB_REFRESH) W.rc = pal_store_refresh(W.msg, sizeof(W.msg));
    else W.rc = pal_store_install(W.id, NULL, NULL, W.msg, sizeof(W.msg));
    W.done_job = W.job;
    W.done = true;
    W.busy = false;
    pal_desktop_invalidate();
}

static void start(int job, const char *id) {
    if (W.busy) return;
    W.job = job;
    nema_strlcpy(W.id, id ? id : "", sizeof(W.id));
    W.busy = true, W.done = false;
    pal_busy_show(job == JOB_REFRESH ? "Reading the catalogue..." : "Downloading and installing...");
    nema_task_desc_t d = {.name = "store", .fn = worker, .stack = 8192, .prio = 3, .core = -1, .role = NEMA_TASK_SYSTEM};
    if (nema_task_create(&d) != NEMA_OK) {
        W.busy = false;
        pal_busy_hide();
        psys_toast("Not enough memory", 1500);
    }
}

/* ---- the form ---- */

static pal_store_entry_t s_list[PAL_STORE_MAX] NEMA_EXT_BSS;
static pal_menu_row_t s_rows[PAL_STORE_MAX + 4] NEMA_EXT_BSS;
static pal_menu_row_t s_drows[10] NEMA_EXT_BSS;
static pal_menu_t s_menu NEMA_EXT_BSS;
static size_t s_n;
static int s_sel = -1; /* the app the detail page shows */
static bool s_loaded;

static void installed_text(const pal_store_entry_t *e, char *out, size_t cap) {
    char v[16];
    pal_store_installed(e->id, v, sizeof(v));
    if (!v[0]) snprintf(out, cap, "%s", e->version);
    else if (!strcmp(v, e->version)) snprintf(out, cap, "installed");
    else snprintf(out, cap, "update %s", e->version);
}

static void row_text(void *ctx, char *out, size_t cap) { installed_text(&s_list[(intptr_t)ctx], out, cap); }
static void refresh_run(void *ctx) {
    (void)ctx;
    start(JOB_REFRESH, NULL);
}

static pal_menu_page_t PG_DETAIL;
static pal_menu_page_t *open_detail(void *ctx) {
    s_sel = (int)(intptr_t)ctx;
    return &PG_DETAIL;
}

static void build_root(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_ACTION, .label = "Refresh", .icon = "store", .run = refresh_run,
                        .hint = "Reads the catalogue again (needs the network)"};
    pal_menu_add(p, &r);
    s_n = 0;
    for (size_t i = 0; i < pal_store_count() && s_n < PAL_STORE_MAX; i++) pal_store_get(i, &s_list[s_n++]);
    if (s_n) {
        r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Apps"};
        pal_menu_add(p, &r);
    }
    for (size_t i = 0; i < s_n; i++) {
        r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = s_list[i].name[0] ? s_list[i].name : s_list[i].id, .icon = "package",
                             .ctx = (void *)(intptr_t)i, .text = row_text, .open = open_detail, .hint = s_list[i].summary};
        pal_menu_add(p, &r);
    }
}

static pal_menu_page_t PG_ROOT = {.title = "Store", .store = s_rows, .cap = PAL_STORE_MAX + 4, .build = build_root,
                                  .empty = "Refresh to read the catalogue"};

static const pal_store_entry_t *sel(void) { return s_sel >= 0 && (size_t)s_sel < s_n ? &s_list[s_sel] : NULL; }
static void d_text(void *ctx, char *out, size_t cap) {
    const pal_store_entry_t *e = sel();
    if (!e) {
        out[0] = 0;
        return;
    }
    switch ((intptr_t)ctx) {
    case 0: snprintf(out, cap, "%s", e->summary); break;
    case 1: snprintf(out, cap, "%s", e->version); break;
    case 2: {
        char v[16];
        pal_store_installed(e->id, v, sizeof(v));
        snprintf(out, cap, "%s", v[0] ? v : "not installed");
        break;
    }
    case 3: snprintf(out, cap, "%lu KB", (unsigned long)(e->size + 1023) / 1024); break;
    default: snprintf(out, cap, "%s", e->id); break;
    }
}
static void install_run(void *ctx) {
    (void)ctx;
    const pal_store_entry_t *e = sel();
    if (e) start(JOB_INSTALL, e->id);
}

static void build_detail(pal_menu_page_t *p) {
    pal_menu_clear(p);
    const pal_store_entry_t *e = sel();
    PG_DETAIL.title = e ? (e->name[0] ? e->name : e->id) : "App";
    if (!e) return;
    static const char *const LABELS[] = {"About", "Version", "Installed", "Size", "Id"};
    for (int i = 0; i < 5; i++) {
        pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = LABELS[i], .text = d_text, .ctx = (void *)(intptr_t)i, .inert = true,
                            .wrap = i == 0};
        pal_menu_add(p, &r);
    }
    char v[16];
    pal_store_installed(e->id, v, sizeof(v));
    pal_menu_row_t r = {.kind = PAL_MENU_ACTION, .label = !v[0] ? "Install" : strcmp(v, e->version) ? "Update" : "Install again",
                        .icon = "store", .run = install_run, .hint = "Downloads it, checks its checksum and installs it"};
    pal_menu_add(p, &r);
}

static pal_menu_page_t PG_DETAIL = {.title = "App", .store = s_drows, .cap = 10, .build = build_detail};

/* ---- the view ---- */

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    pal_menu_draw(&s_menu, c, a, pal_theme_active());
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (W.busy) return true;
    bool used = pal_menu_key(&s_menu, k);
    if (used) {
        pal_menu_follow(&s_menu);
        pal_desktop_invalidate();
    }
    return used;
}

static void pump(pal_view_t *v) {
    (void)v;
    if (!W.done) return;
    W.done = false;
    pal_busy_hide();
    if (W.rc == NEMA_OK) psys_toast(W.done_job == JOB_REFRESH ? "Catalogue read" : "Installed: it is in the launcher", 1500);
    else psys_toast(W.msg[0] ? W.msg : "Failed", 2500);
    pal_menu_refresh(&s_menu);
    pal_desktop_invalidate();
}

static void enter(pal_view_t *v) {
    (void)v;
    if (!s_loaded) {
        s_loaded = true;
        pal_menu_init(&s_menu, &PG_ROOT);
        start(JOB_REFRESH, NULL); /* the first visit reads the catalogue */
    }
    pal_menu_bind("store", &s_menu);
}

pal_view_t pal_store_view = {.id = "store", .title = "Store", .enter = enter, .draw = draw, .key = key, .pump = pump};

/* ---- the command ---- */

/* store [list | refresh | info ID | install ID | update]: the catalogue from the shell. */
static int cmd_store(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "list";
    char msg[96];
    if (!strcmp(sub, "list") || !strcmp(sub, "refresh") || !strcmp(sub, "update")) {
        if (pal_store_refresh(msg, sizeof(msg)) != NEMA_OK) {
            pal_eprintf("store: %s\n", msg);
            return 1;
        }
        size_t n = pal_store_count(), updated = 0;
        for (size_t i = 0; i < n; i++) {
            pal_store_entry_t e;
            pal_store_get(i, &e);
            char v[16];
            pal_store_installed(e.id, v, sizeof(v));
            bool newer = v[0] && strcmp(v, e.version);
            if (!strcmp(sub, "update")) {
                if (!newer) continue;
                pal_printf("updating %s %s -> %s...\n", e.id, v, e.version);
                if (pal_store_install(e.id, NULL, NULL, msg, sizeof(msg)) != NEMA_OK) pal_eprintf("store: %s: %s\n", e.id, msg);
                else updated++;
                continue;
            }
            pal_printf("%-16s %-8s %-10s %s\n", e.id, e.version, !v[0] ? "" : newer ? "update" : "installed", e.summary);
        }
        if (!strcmp(sub, "update")) pal_printf("%u updated\n", (unsigned)updated);
        else if (!n) pal_printf("the catalogue is empty\n");
        return 0;
    }
    if ((!strcmp(sub, "info") || !strcmp(sub, "install")) && argc > 2) {
        if (!pal_store_count() && pal_store_refresh(msg, sizeof(msg)) != NEMA_OK) {
            pal_eprintf("store: %s\n", msg);
            return 1;
        }
        pal_store_entry_t e;
        if (!pal_store_find(argv[2], &e)) {
            pal_eprintf("store: %s is not in the catalogue\n", argv[2]);
            return 1;
        }
        if (!strcmp(sub, "info")) {
            pal_printf("%s (%s) %s, %lu bytes\n%s\n", e.name, e.id, e.version, (unsigned long)e.size, e.summary);
            return 0;
        }
        pal_printf("installing %s %s...\n", e.id, e.version);
        if (pal_store_install(e.id, NULL, NULL, msg, sizeof(msg)) != NEMA_OK) {
            pal_eprintf("store: %s\n", msg);
            return 1;
        }
        pal_printf("installed: run it by typing %s\n", e.id);
        return 0;
    }
    pal_eprintf("usage: store [list | refresh | info ID | install ID | update]\n");
    return 2;
}

static const nema_app_t STORE_APPS[] = {{"store", "the app catalogue: list, info, install, update", cmd_store, 12288, 0}};

void pal_apps_register_store(void);
void pal_apps_register_store(void) { nema_app_register(&STORE_APPS[0]); }
