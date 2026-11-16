/* The registry of apps: listing, the launcher's entries, and admission (the limits, counted from live processes).
 * Installing and removing packages is in test_papp.c. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "pal/registry.h"
#include "test.h"

static const pal_reg_builtin_t BUILTIN[] = {
    {"launcher", "Home", "1.0.0", 2, PAL_KIND_GUI, true, 0, 0, 0, 0, "", "System"},
    {"terminal", "Terminal", "1.0.0", 2, PAL_KIND_GUI, false, 0, 0, 0, 0, "", "Tools"},
    {"monitor", "Monitor", "1.0.0", 2, PAL_KIND_GUI, false, 0, 0, 0, 0, "", "System"},
};
static const pal_reg_defaults_t DEFAULTS = {PAL_KIND_TTY, true, 2};

static nema_owner_t OW;

/* A command that blocks on stdin until it is killed. */
static int app_block(int argc, char **argv) {
    char b[4];
    nema_read(0, b, sizeof(b));
    return 0;
}

static nema_app_t CMD_SLEEP = {"sleepcmd", "test command (hidden by default)", app_block, 8192, 0};

static pal_papp_t fake_app(const char *id, pal_kind_t kind) {
    pal_papp_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id, sizeof(a.id), "%s", id);
    snprintf(a.name, sizeof(a.name), "%s", id);
    snprintf(a.version, sizeof(a.version), "0.1");
    a.api = 2;
    a.kind = kind;
    a.perms = PAL_PERM_NET;
    return a;
}

static bool has_id(const char *id, size_t n, pal_reg_app_t *list) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(list[i].id, id) == 0) return true;
    return false;
}

TEST(registry_lists_views_then_installed_then_commands) {
    CHECK_EQ(psys_reg_count(), 3 + 1); /* three views and the sleep command */
    pal_reg_app_t e;
    CHECK(psys_reg_get(0, &e) && strcmp(e.id, "launcher") == 0 && e.source == PAL_SRC_BUILTIN);
    CHECK(psys_reg_get(1, &e) && strcmp(e.id, "terminal") == 0 && e.kind == PAL_KIND_GUI);
    CHECK(psys_reg_find("sleepcmd", &e) && e.kind == PAL_KIND_TTY && e.hidden);
    CHECK(!psys_reg_find("nope", &e));
}

TEST(launcher_lists_visible_views_and_installed_tty_and_gui_only) {
    pal_papp_t hello = fake_app("hello", PAL_KIND_TTY), dash = fake_app("dash", PAL_KIND_GUI), svc = fake_app("svc1", PAL_KIND_SERVICE);
    CHECK_EQ(psys_reg_add_installed(&hello, "/apps/hello.papp"), NEMA_OK);
    CHECK_EQ(psys_reg_add_installed(&dash, "/apps/dash/app.papp"), NEMA_OK);
    CHECK_EQ(psys_reg_add_installed(&svc, "/apps/svc1/app.papp"), NEMA_OK);
    CHECK_EQ(psys_reg_add_installed(&hello, "/apps/hello.papp"), NEMA_ERR_EXIST);

    /* the launcher's rule: not hidden, and kind tty or gui. Views: terminal, monitor (launcher is hidden). Installed: hello, dash. */
    size_t n = psys_launcher_count();
    CHECK_EQ(n, 4);
    pal_reg_app_t list[8];
    for (size_t i = 0; i < n && i < 8; i++) CHECK(psys_launcher_get(i, &list[i]));
    CHECK(has_id("terminal", n, list) && has_id("monitor", n, list) && has_id("hello", n, list) && has_id("dash", n, list));
    CHECK(!has_id("launcher", n, list)); /* hidden view */
    CHECK(!has_id("svc1", n, list));     /* services are not launchable from the home screen */
    CHECK(!has_id("sleepcmd", n, list)); /* hidden command */
    CHECK(!psys_launcher_get(n, &list[0]));
    /* the service is still in the registry, with its source and path */
    pal_reg_app_t e;
    CHECK(psys_reg_find("svc1", &e) && e.source == PAL_SRC_INSTALLED && e.kind == PAL_KIND_SERVICE);
    CHECK(strcmp(e.path, "/apps/svc1/app.papp") == 0);
    CHECK_EQ(psys_reg_remove("svc1"), NEMA_OK);
    CHECK_EQ(psys_reg_remove("hello"), NEMA_OK);
    CHECK_EQ(psys_reg_remove("dash"), NEMA_OK);
    CHECK_EQ(psys_reg_remove("dash"), NEMA_ERR_NOENT);
    CHECK_EQ(psys_reg_remove("terminal"), NEMA_ERR_NOENT); /* built-ins are never removed from the registry */
    CHECK_EQ(psys_launcher_count(), 2);
}

TEST(a_command_with_a_view_id_is_listed_once) {
    nema_app_t dup = {"monitor", "shadowed", NULL, 0, 0};
    nema_app_register(&dup);
    size_t before = psys_reg_count();
    CHECK_EQ(before, 3 + 1); /* views (3) + sleepcmd; the monitor command is shadowed by the view */
    nema_app_unregister("monitor");
}

TEST(limits_are_the_named_constants) {
    CHECK_EQ(psys_reg_check_limits(PAL_KIND_TTY, PSYS_REG_MAX_WASM - 1, 0), NEMA_OK);
    CHECK_EQ(psys_reg_check_limits(PAL_KIND_TTY, PSYS_REG_MAX_WASM, 0), NEMA_ERR_FULL); /* the 5th app is refused with 4 */
    CHECK_EQ(psys_reg_check_limits(PAL_KIND_SERVICE, 0, PSYS_REG_MAX_SERVICES - 1), NEMA_OK);
    CHECK_EQ(psys_reg_check_limits(PAL_KIND_SERVICE, 0, PSYS_REG_MAX_SERVICES), NEMA_ERR_FULL);
    CHECK_EQ(psys_reg_check_limits(PAL_KIND_TTY, 0, PSYS_REG_MAX_SERVICES), NEMA_OK); /* services limit is for services */
    CHECK_EQ(PSYS_REG_MAX_WASM, 4);
    CHECK_EQ(PSYS_REG_MAX_SERVICES, 6);
    CHECK_EQ(PSYS_REG_MAX_GUI_FOCUS, 1);
}

/* Four installed apps really running (blocked on a pipe), then a fifth: refused with the message; one stopped, it is let in. */
TEST(admission_counts_running_installed_apps) {
    static const char *const names[5] = {"wasm1", "wasm2", "wasm3", "wasm4", "wasm5"};
    static nema_app_t cmds[5];
    nema_pid_t pids[5];
    nema_obj_t *wr[5];
    for (int i = 0; i < 5; i++) {
        pal_papp_t a = fake_app(names[i], PAL_KIND_TTY);
        CHECK_EQ(psys_reg_add_installed(&a, "/apps/x.papp"), NEMA_OK);
        cmds[i] = (nema_app_t){names[i], "installed test app", app_block, 8192, 0};
        CHECK_EQ(nema_app_register(&cmds[i]), NEMA_OK);
    }
    for (int i = 0; i < 4; i++) {
        nema_obj_t *rd;
        nema_pipe_create(64, NEMA_MEM_ANY, OW, &rd, &wr[i]);
        nema_spawn_t sp = {.name = names[i], .stdio = {rd, NULL, NULL}};
        CHECK_EQ(nema_proc_spawn(&sp, &pids[i]), NEMA_OK);
        nema_obj_unref(rd);
    }
    pal_reg_app_t e;
    CHECK(psys_reg_find("wasm5", &e));
    char msg[120] = "";
    CHECK_EQ(psys_reg_admit(&e, 0, msg, sizeof(msg)), NEMA_ERR_FULL);
    CHECK(strstr(msg, "limit 4") != NULL);
    /* stop one: the fifth is admitted */
    nema_proc_signal(pids[0], NEMA_SIG_KILL);
    int code;
    nema_proc_wait(pids[0], &code, 2000);
    CHECK_EQ(psys_reg_admit(&e, 0, msg, sizeof(msg)), NEMA_OK);
    for (int i = 1; i < 4; i++) {
        nema_proc_signal(pids[i], NEMA_SIG_KILL);
        nema_proc_wait(pids[i], &code, 2000);
    }
    for (int i = 0; i < 4; i++) nema_obj_unref(wr[i]);
    for (int i = 0; i < 5; i++) {
        nema_app_unregister(names[i]);
        psys_reg_remove(names[i]);
    }
}

int main(void) {
    nema_config_t cfg = {0};
    nema_init(&cfg);
    OW = nema_owner_register("test_registry");
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(1024u * 1024u));
    nema_app_register(&CMD_SLEEP);
    psys_reg_init(BUILTIN, sizeof(BUILTIN) / sizeof(BUILTIN[0]), &DEFAULTS);
    RUN(registry_lists_views_then_installed_then_commands);
    RUN(launcher_lists_visible_views_and_installed_tty_and_gui_only);
    RUN(a_command_with_a_view_id_is_listed_once);
    RUN(limits_are_the_named_constants);
    RUN(admission_counts_running_installed_apps);
    return TEST_DONE();
}
