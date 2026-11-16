/* The generated import table of the palanu module, end to end: hand-encoded wasm apps (tests/unit/wasm_fixtures_api.h,
 * made by tests/unit/tools/gen_api_fixtures.py) call palanu.open and palanu.tcp_connect, and exit with the result. The
 * capability check in the glue and the namespace in system/api run as they do for a real app. */
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "test.h"
#include "wasm_fixtures_api.h"

#define PERM_FS 1
#define PERM_NET 2
#define PERM_HOME 4

/* The app side: argv[1] is the module (open or connect), argv[2] the manifest permissions. */
static int glue_app(int argc, char **argv) {
    if (argc < 3) return 99;
    const uint8_t *mod = strcmp(argv[1], "open") == 0 ? WASM_OPEN_HOME : WASM_CONNECT;
    size_t n = strcmp(argv[1], "open") == 0 ? sizeof(WASM_OPEN_HOME) : sizeof(WASM_CONNECT);
    uint8_t *img = (uint8_t *)nema_alloc(NEMA_MEM_ANY, nema_self()->owner, n);
    if (!img) return 98;
    memcpy(img, mod, n);
    pal_papp_t app;
    memset(&app, 0, sizeof(app));
    strcpy(app.id, "glue");
    app.api = 2;
    app.perms = (uint32_t)strtoul(argv[2], NULL, 10);
    app.mem_kb = 64;
    app.stack_kb = 16;
    app.payload = img;
    app.payload_len = n;
    return pal_wasm_run(&app, img, argc, argv);
}

static const nema_app_t GLUE = {"glue-app", "wasm import table scenarios (tests)", glue_app, 0, NEMA_APP_HIDDEN};

static int run(const char *module, uint32_t perms) {
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%u", perms);
    const char *av[] = {"glue-app", module, pbuf};
    nema_spawn_t sp = {.name = "glue-app", .argc = 3, .argv = av};
    nema_pid_t pid;
    if (nema_proc_spawn(&sp, &pid) != NEMA_OK) return -1000;
    int code = -1;
    if (nema_proc_wait(pid, &code, 10000) != NEMA_OK) return -1001;
    return code;
}

/* open("/home/x") with only fs: the glue lets it through (fs), the namespace refuses /home (fs.home). */
TEST(open_home_is_refused_without_fs_home) { CHECK_EQ(run("open", PERM_FS), -9); }

/* with fs.home the namespace reaches /home: the file is not there, so the answer is "not found". */
TEST(open_home_reaches_home_with_fs_home) { CHECK_EQ(run("open", PERM_FS | PERM_HOME), -3); }

/* without the fs capability the glue refuses the import before the namespace is consulted. */
TEST(open_is_refused_by_the_glue_without_fs) { CHECK_EQ(run("open", 0), -9); }

/* tcp_connect without the net capability: refused by the glue. */
TEST(connect_is_refused_without_net) { CHECK_EQ(run("connect", 0), -9); }

/* with net, a closed loopback port answers "connection refused" through the import. */
TEST(connect_with_net_reports_refused) { CHECK_EQ(run("connect", PERM_NET), NEMA_ERR_REFUSED); }

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_net_init();
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(1024u * 1024u));
    nema_app_register(&GLUE);
    RUN(open_home_is_refused_without_fs_home);
    RUN(open_home_reaches_home_with_fs_home);
    RUN(open_is_refused_by_the_glue_without_fs);
    RUN(connect_is_refused_without_net);
    RUN(connect_with_net_reports_refused);
    return TEST_DONE();
}
