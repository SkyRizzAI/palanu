/* The fs namespace of the palanu API, through the psys_api_* functions as a process runs them. Each scenario is
 * a native app process with its own api context (the handles and the private /tmp belong to one run). The scenario
 * name is argv[1], the app id argv[2], the manifest permissions argv[3]. The process exit status is the number of
 * failed checks inside it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../system/src/api_ctx.h"
#include "../../system/src/api_impl.h"
#include "nema/nema.h"
#include "pal/papp.h"
#include "test.h"

/* Record layouts and kinds (api/palanu/fs.h). */
#define S_KIND_FILE 1
#define S_KIND_DIR 2

static int g_bad;
#define AC(cond)                                                          \
    do {                                                                  \
        if (!(cond)) {                                                    \
            g_bad++;                                                      \
            printf("    app check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                 \
    } while (0)

static int32_t op(api_ctx_t *c, const char *p, int32_t fl) { return psys_api_open(c, p, (uint32_t)strlen(p), fl); }
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t put_text(api_ctx_t *c, const char *p, const char *text) {
    int32_t fd = op(c, p, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC);
    if (fd < 0) return fd;
    psys_api_write(c, fd, text, (uint32_t)strlen(text));
    return psys_api_close(c, fd);
}

/* ---- scenarios ---- */

static void sc_home_denied(api_ctx_t *c) {
    AC(op(c, "/home/.ssh/id", NEMA_O_READ) == NEMA_ERR_PERM); /* the key the namespace exists to keep out */
    AC(op(c, "/home/x", NEMA_O_WRITE | NEMA_O_CREATE) == NEMA_ERR_PERM);
    uint8_t st[16];
    AC(psys_api_stat(c, "/home", st, sizeof st) == NEMA_ERR_PERM);
    AC(psys_api_mkdir(c, "/home/new") == NEMA_ERR_PERM);
    AC(psys_api_rename(c, "/data/a", "/home/a") == NEMA_ERR_PERM);
    AC(op(c, "/sd/x", NEMA_O_READ) == NEMA_ERR_PERM); /* fs.media is not granted */
    AC(op(c, "/etc/passwd", NEMA_O_READ) == NEMA_ERR_PERM);
    AC(op(c, "/apps/appb.papp", NEMA_O_READ) == NEMA_ERR_PERM); /* another app's package */
    AC(op(c, "/bin/ls", NEMA_O_READ) == NEMA_ERR_PERM);
    AC(psys_api_stat(c, "/home/.ssh", st, sizeof st) == NEMA_ERR_PERM);
    /* the root lists only the roots this app may see */
    int32_t dh = psys_api_dir_open(c, "/");
    AC(dh >= 0);
    uint8_t ent[64];
    int names = 0;
    int32_t r;
    while ((r = psys_api_dir_next(c, dh, ent, sizeof ent)) == 1) {
        const char *n = (const char *)ent + 8;
        AC(strcmp(n, "data") == 0 || strcmp(n, "tmp") == 0);
        names++;
    }
    AC(r == 0);
    AC(names == 2);
    psys_api_close(c, dh);
}

static void sc_home_granted(api_ctx_t *c) {
    AC(op(c, "/home/.ssh/id", NEMA_O_READ) >= 0); /* with fs.home the file is reachable (the test wrote it) */
    int32_t fd = op(c, "/home/from-app", NEMA_O_WRITE | NEMA_O_CREATE);
    AC(fd >= 0);
    psys_api_close(c, fd);
    AC(psys_api_remove(c, "/home/from-app") == 0);
    AC(op(c, "/sd/x", NEMA_O_READ) == NEMA_ERR_PERM); /* fs.media still not granted */
}

static void sc_dotdot(api_ctx_t *c) {
    AC(op(c, "/data/../../home/.ssh/id", NEMA_O_READ) == NEMA_ERR_PERM);
    AC(op(c, "/../home/.ssh/id", NEMA_O_READ) == NEMA_ERR_PERM);
    AC(op(c, "/data/..", NEMA_O_READ) == NEMA_ERR_ISDIR); /* "/data/.." is the root, which is a directory */
    AC(op(c, "/data/../..", NEMA_O_READ) == NEMA_ERR_PERM); /* the second ".." leaves the root: refused */
    AC(op(c, "../../home/.ssh/id", NEMA_O_READ) == NEMA_ERR_PERM); /* relative: starts in /data, so this climbs out */
    uint8_t st[16];
    AC(psys_api_stat(c, "/data/../../x", st, sizeof st) == NEMA_ERR_PERM);
    AC(put_text(c, "/data//inner/../f.txt", "dots") == 0); /* collapses: /data/f.txt */
    AC(psys_api_stat(c, "/data/./f.txt", st, sizeof st) == 0);
    AC(psys_api_remove(c, "/data/f.txt") == 0);
}

static void sc_persist_write(api_ctx_t *c) { AC(put_text(c, "/data/keep", "kept") == 0); }

static void sc_persist_check(api_ctx_t *c) {
    int32_t fd = op(c, "/data/keep", NEMA_O_READ);
    AC(fd >= 0);
    char b[16] = {0};
    AC(psys_api_read(c, fd, b, sizeof b) == 4);
    AC(strcmp(b, "kept") == 0);
    psys_api_close(c, fd);
}

static void sc_persist_absent(api_ctx_t *c) { /* another app: its own /data does not hold the file */
    AC(op(c, "/data/keep", NEMA_O_READ) == NEMA_ERR_NOENT);
    uint8_t st[16];
    AC(psys_api_stat(c, "/data/keep", st, sizeof st) == NEMA_ERR_NOENT);
}

static void sc_tmp_write(api_ctx_t *c) {
    AC(put_text(c, "/tmp/scratch", "temp") == 0);
    int32_t dh = psys_api_dir_open(c, "/tmp");
    AC(dh >= 0);
    uint8_t ent[64];
    AC(psys_api_dir_next(c, dh, ent, sizeof ent) == 1);
    AC(strcmp((const char *)ent + 8, "scratch") == 0);
    AC(psys_api_dir_next(c, dh, ent, sizeof ent) == 0);
    psys_api_close(c, dh);
}

static void sc_tmp_hold(api_ctx_t *c) {
    AC(put_text(c, "/tmp/held", "x") == 0);
    nema_sleep_ms(400); /* the other app looks at its own /tmp meanwhile */
}

static void sc_tmp_peek(api_ctx_t *c) {
    int32_t dh = psys_api_dir_open(c, "/tmp");
    AC(dh >= 0);
    uint8_t ent[64];
    AC(psys_api_dir_next(c, dh, ent, sizeof ent) == 0); /* empty: "held" belongs to the other app */
    psys_api_close(c, dh);
    uint8_t st[16];
    AC(psys_api_stat(c, "/tmp/held", st, sizeof st) == NEMA_ERR_NOENT);
}

static void sc_ops(api_ctx_t *c) {
    AC(psys_api_mkdir(c, "/data/d") == 0);
    AC(psys_api_mkdir(c, "/data/d") == NEMA_ERR_EXIST);
    AC(psys_api_mkdir(c, "/data/nope/deep") == NEMA_ERR_NOENT);
    int32_t fd = op(c, "/data/d/f", NEMA_O_READ | NEMA_O_WRITE | NEMA_O_CREATE);
    AC(fd >= 0);
    AC(psys_api_write(c, fd, "0123456789", 10) == 10);
    uint8_t st[16];
    AC(psys_api_stat(c, "/data/d/f", st, sizeof st) == 0);
    AC(u32(st) == 10 && u32(st + 8) == S_KIND_FILE);
    AC(psys_api_stat(c, "/data/d", st, sizeof st) == 0 && u32(st + 8) == S_KIND_DIR);
    AC(psys_api_stat(c, "/data/d", st, 8) == NEMA_ERR_INVAL); /* the record needs 16 bytes */
    /* seek: absolute, relative, from the end */
    AC(psys_api_seek(c, fd, 4, 0) == 4);
    AC(psys_api_seek(c, fd, 2, 1) == 6);
    AC(psys_api_seek(c, fd, -1, 2) == 9);
    char b[4] = {0};
    AC(psys_api_read(c, fd, b, 1) == 1 && b[0] == '9');
    AC(psys_api_seek(c, fd, -100, 0) < 0);
    AC(psys_api_seek(c, fd, 0, 7) == NEMA_ERR_INVAL);
    AC(psys_api_fsync(c, fd) == 0);
    psys_api_close(c, fd);
    AC(psys_api_seek(c, fd, 0, 0) == NEMA_ERR_BADF); /* closed */
    AC(psys_api_fsync(c, fd) == NEMA_ERR_BADF);
    /* directory listing, and a directory handle is not a file handle */
    int32_t dh = psys_api_dir_open(c, "/data/d");
    AC(dh >= 0);
    uint8_t ent[64];
    AC(psys_api_dir_next(c, dh, ent, sizeof ent) == 1);
    AC(strcmp((const char *)ent + 8, "f") == 0 && u32(ent + 4) == S_KIND_FILE && u32(ent) == 10);
    AC(psys_api_dir_next(c, dh, ent, sizeof ent) == 0);
    AC(psys_api_dir_next(c, dh, ent, 10) == NEMA_ERR_INVAL);
    AC(psys_api_seek(c, dh, 0, 0) == NEMA_ERR_ISDIR);
    AC(psys_api_fsync(c, dh) == NEMA_ERR_ISDIR);
    psys_api_close(c, dh);
    AC(psys_api_dir_open(c, "/data/d/f") == NEMA_ERR_NOTDIR);
    AC(op(c, "/data/d", NEMA_O_READ) == NEMA_ERR_ISDIR);
    /* rename, remove */
    AC(psys_api_rename(c, "/data/d/f", "/data/d/g") == 0);
    AC(psys_api_stat(c, "/data/d/f", st, sizeof st) == NEMA_ERR_NOENT);
    AC(psys_api_stat(c, "/data/d/g", st, sizeof st) == 0);
    AC(psys_api_remove(c, "/data/d") < 0); /* not empty */
    AC(psys_api_remove(c, "/data/d/g") == 0);
    AC(psys_api_remove(c, "/data/d") == 0);
    AC(psys_api_stat(c, "/data/d", st, sizeof st) == NEMA_ERR_NOENT);
    AC(psys_api_remove(c, "/") == NEMA_ERR_PERM);
}

/* The app's own table holds 16 handles. The process table under it holds NEMA_HANDLES_MAX, stdio included, so the
 * app gets the smaller of the two: that is what this checks. */
static void sc_handles(api_ctx_t *c) {
    int32_t fds[16];
    int n = 0;
    while (n < 16 && (fds[n] = op(c, "/data/h", NEMA_O_READ | NEMA_O_WRITE | NEMA_O_CREATE)) >= 0) n++;
    AC(n == (NEMA_HANDLES_MAX - 3 < 16 ? NEMA_HANDLES_MAX - 3 : 16));
    AC(op(c, "/data/h", NEMA_O_READ) < 0); /* no more room */
    AC(psys_api_dir_open(c, "/data") < 0); /* directories share the table */
    for (int i = 0; i < n; i++) psys_api_close(c, fds[i]);
    int32_t dh = psys_api_dir_open(c, "/data");
    AC(dh >= 0);
    psys_api_close(c, dh);
}

static const struct {
    const char *name;
    void (*fn)(api_ctx_t *);
} SCENARIOS[] = {
    {"home_denied", sc_home_denied},     {"home_granted", sc_home_granted},   {"dotdot", sc_dotdot},
    {"persist_write", sc_persist_write}, {"persist_check", sc_persist_check}, {"persist_absent", sc_persist_absent},
    {"tmp_write", sc_tmp_write},         {"tmp_hold", sc_tmp_hold},           {"tmp_peek", sc_tmp_peek},
    {"ops", sc_ops},                     {"handles", sc_handles},
};

/* The app side: a process that runs one scenario in its own context. */
static int fsapp_main(int argc, char **argv) {
    if (argc < 4) return 99;
    pal_papp_t app;
    memset(&app, 0, sizeof(app));
    snprintf(app.id, sizeof(app.id), "%s", argv[2]);
    app.api = 2;
    app.perms = (uint32_t)strtoul(argv[3], NULL, 10);
    api_ctx_t *c = api_ctx_new(&app, argc, argv);
    if (!c) return 98;
    g_bad = 0;
    int found = 0;
    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); i++) {
        if (strcmp(argv[1], SCENARIOS[i].name) == 0) {
            SCENARIOS[i].fn(c);
            found = 1;
        }
    }
    api_ctx_free(c);
    return found ? g_bad : 97;
}

static const nema_app_t FSAPP = {"fsapp", "fs namespace scenarios (tests)", fsapp_main, 0, NEMA_APP_HIDDEN};

/* ---- the driver: spawns a scenario and returns its exit status ---- */
#define PERM_FS 1u
#define PERM_HOME 4u
#define PERM_MEDIA 8u

static int run(const char *scen, const char *id, uint32_t perms) {
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%u", perms);
    const char *av[] = {"fsapp", scen, id, pbuf};
    nema_spawn_t sp = {.name = "fsapp", .argc = 4, .argv = av};
    nema_pid_t pid;
    if (nema_proc_spawn(&sp, &pid) != NEMA_OK) return -1000;
    int code = -1;
    if (nema_proc_wait(pid, &code, 10000) != NEMA_OK) return -1001;
    return code;
}

static void setup_home(void) {
    nema_fs_mkdir("/home");
    nema_fs_mkdir("/home/.ssh");
    nema_obj_t *f;
    if (nema_fs_open("/home/.ssh/id", NEMA_O_WRITE | NEMA_O_CREATE, &f) == NEMA_OK) {
        nema_obj_write(f, "PRIVATE KEY", 11, 0);
        nema_obj_unref(f);
    }
}

TEST(a_legacy_or_plain_app_cannot_read_home_keys) {
    CHECK_EQ(run("home_denied", "appa", PERM_FS), 0);
}

TEST(fs_home_grants_home_and_nothing_more) {
    CHECK_EQ(run("home_granted", "appa", PERM_FS | PERM_HOME), 0);
}

TEST(dotdot_cannot_escape_the_namespace) {
    CHECK_EQ(run("dotdot", "appa", PERM_FS), 0);
}

TEST(data_persists_across_runs_and_is_per_app) {
    CHECK_EQ(run("persist_write", "appa", PERM_FS), 0);
    CHECK_EQ(run("persist_check", "appa", PERM_FS), 0); /* a second run sees it */
    CHECK_EQ(run("persist_absent", "appb", PERM_FS), 0); /* another app does not */
}

TEST(tmp_is_removed_when_the_app_ends) {
    CHECK_EQ(run("tmp_write", "appa", PERM_FS), 0);
    int leftovers = 0;
    nema_dirent_t e;
    for (size_t i = 0; nema_fs_readdir("/tmp/.app", i, &e) == NEMA_OK; i++)
        if (strncmp(e.name, "appa-", 5) == 0) leftovers++;
    CHECK_EQ(leftovers, 0);
}

TEST(tmp_is_private_to_the_running_app) {
    nema_spawn_t hold_sp = {.name = "fsapp"};
    const char *av[] = {"fsapp", "tmp_hold", "appa", "1"};
    hold_sp.argc = 4;
    hold_sp.argv = av;
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&hold_sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(100);
    CHECK_EQ(run("tmp_peek", "appb", PERM_FS), 0);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 5000) == NEMA_OK);
    CHECK_EQ(code, 0);
}

TEST(stat_seek_readdir_mkdir_rename_remove_behave) {
    CHECK_EQ(run("ops", "appa", PERM_FS), 0);
}

TEST(a_run_holds_its_handle_limit_and_no_more) {
    CHECK_EQ(run("handles", "appa", PERM_FS), 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(2u * 1024u * 1024u));
    setup_home();
    nema_app_register(&FSAPP);
    RUN(a_legacy_or_plain_app_cannot_read_home_keys);
    RUN(fs_home_grants_home_and_nothing_more);
    RUN(dotdot_cannot_escape_the_namespace);
    RUN(data_persists_across_runs_and_is_per_app);
    RUN(tmp_is_removed_when_the_app_ends);
    RUN(tmp_is_private_to_the_running_app);
    RUN(stat_seek_readdir_mkdir_rename_remove_behave);
    RUN(a_run_holds_its_handle_limit_and_no_more);
    return TEST_DONE();
}
