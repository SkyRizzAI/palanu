/* The kv, settings and notify modules of the palanu API, end to end: hand-encoded wasm apps (tests/unit/
 * wasm_fixtures_svc.h, made by tests/unit/tools/gen_svc_fixtures.py) run as processes through the generated import table.
 * The glue checks the capability (settings.read, settings.write, notify) before the implementation runs, and each app
 * exits with the result of its last call. Each app has its own id, so kv isolation is checked here too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "test.h"
#include "wasm_fixtures_svc.h"

#define PERM_SETTINGS_READ 0x80
#define PERM_SETTINGS_WRITE 0x100
#define PERM_NOTIFY 0x200

/* The app side: argv[1] names the fixture, argv[2] the app id, argv[3] the manifest permissions. */
static int svc_app(int argc, char **argv) {
    if (argc < 4) return 99;
    const svc_fixture_t *fx = NULL;
    for (size_t i = 0; i < sizeof(SVC_FIXTURES) / sizeof(SVC_FIXTURES[0]); i++)
        if (strcmp(SVC_FIXTURES[i].name, argv[1]) == 0) fx = &SVC_FIXTURES[i];
    if (!fx) return 98;
    uint8_t *img = (uint8_t *)nema_alloc(NEMA_MEM_ANY, nema_self()->owner, fx->len);
    if (!img) return 97;
    memcpy(img, fx->mod, fx->len);
    pal_papp_t app;
    memset(&app, 0, sizeof(app));
    snprintf(app.id, sizeof(app.id), "%s", argv[2]);
    app.api = 2;
    app.perms = (uint32_t)strtoul(argv[3], NULL, 10);
    app.mem_kb = 64;
    app.stack_kb = 16;
    app.payload = img;
    app.payload_len = fx->len;
    return pal_wasm_run(&app, img, 0, NULL);
}

static const nema_app_t SVC_APP = {"svc-app", "kv, settings and notify scenarios (tests)", svc_app, 0, NEMA_APP_HIDDEN};

/* Runs fixture in the app id with perms; returns its exit status (the last call's result), or a negative test error. */
static int run(const char *fixture, const char *id, uint32_t perms) {
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%u", perms);
    const char *av[] = {"svc-app", fixture, id, pbuf};
    nema_spawn_t sp = {.name = "svc-app", .argc = 4, .argv = av};
    nema_pid_t pid;
    if (nema_proc_spawn(&sp, &pid) != NEMA_OK) return -1000;
    int code = -1;
    if (nema_proc_wait(pid, &code, 20000) != NEMA_OK) return -1001;
    return code;
}

/* ---- kv ---- */

TEST(kv_set_and_get_across_runs_of_one_app) {
    CHECK_EQ(run("SVC_KV_SET", "kva", 0), 0);
    CHECK_EQ(run("SVC_KV_GET", "kva", 0), 4); /* "blue" */
    CHECK_EQ(run("SVC_KV_FIRST", "kva", 0), 'b');
}

TEST(kv_is_per_app) {
    CHECK_EQ(run("SVC_KV_GET", "kvb", 0), NEMA_ERR_NOENT); /* another app does not see it */
    CHECK_EQ(run("SVC_KV_GET", "kva", 0), 4);
}

TEST(kv_refuses_a_bad_key_and_a_65th_key) {
    CHECK_EQ(run("SVC_KV_BAD_KEY", "kva", 0), NEMA_ERR_INVAL);
    CHECK_EQ(run("SVC_KV_FILL", "kvfill", 0), NEMA_ERR_NOSPC);
}

/* ---- settings: the capability is checked by the glue ---- */

TEST(setting_get_needs_settings_read) {
    CHECK_EQ(run("SVC_SETTING_GET", "sg", 0), NEMA_ERR_PERM);
    CHECK_EQ(run("SVC_SETTING_GET", "sg", PERM_SETTINGS_READ), 1); /* "0" or "1" */
    int c = run("SVC_SETTING_FIRST", "sg", PERM_SETTINGS_READ);
    CHECK(c == '0' || c == '1');
}

TEST(setting_set_needs_settings_write_and_refuses_read_only) {
    CHECK_EQ(run("SVC_SETTING_SET_DARK", "sw", 0), NEMA_ERR_PERM);
    CHECK_EQ(run("SVC_SETTING_SET_DARK", "sw", PERM_SETTINGS_READ), NEMA_ERR_PERM);
    CHECK_EQ(run("SVC_SETTING_SET_DARK", "sw", PERM_SETTINGS_WRITE), NEMA_OK);
    CHECK_EQ(run("SVC_SETTING_SET_RO", "sw", PERM_SETTINGS_WRITE), NEMA_ERR_PERM); /* radio.ble is read only */
}

/* ---- notify ---- */

TEST(notify_post_needs_the_notify_permission) {
    CHECK_EQ(run("SVC_NOTIFY_POST", "wn", 0), NEMA_ERR_PERM);
    CHECK(run("SVC_NOTIFY_POST", "wn", PERM_NOTIFY) > 0); /* the id */
}

TEST(notify_rate_limit_refuses_the_fifth_post) {
    CHECK_EQ(run("SVC_NOTIFY_RATE", "wr", PERM_NOTIFY), NEMA_ERR_BUSY);
}

TEST(toast_checks_its_duration) {
    CHECK_EQ(run("SVC_TOAST_SHORT", "wt", PERM_NOTIFY), NEMA_ERR_INVAL);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(2u * 1024u * 1024u));
    nema_app_register(&SVC_APP);
    RUN(kv_set_and_get_across_runs_of_one_app);
    RUN(kv_is_per_app);
    RUN(kv_refuses_a_bad_key_and_a_65th_key);
    RUN(setting_get_needs_settings_read);
    RUN(setting_set_needs_settings_write_and_refuses_read_only);
    RUN(notify_post_needs_the_notify_permission);
    RUN(notify_rate_limit_refuses_the_fifth_post);
    RUN(toast_checks_its_duration);
    return TEST_DONE();
}
