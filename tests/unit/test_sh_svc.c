/* The service commands (apps/cmd_svc.c): svc list, status, start, stop, enable, logs; safemode; boot. Each runs as a real
 * process with its output on a pipe, as the shell runs it. The services are fakes registered with the supervisor. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/bootcount.h"
#include "pal/svc.h"
#include "test.h"

void pal_apps_register_svc(void);
void pal_apps_register_perf(void);

static nema_owner_t OW;
static uint64_t g_now;
static uint64_t fake_clock(void) { return g_now; }

static nema_err_t f_ok(void) { return NEMA_OK; }
static void f_stop(void) {}

static void reg(const char *id, bool autostart, bool core) {
    psys_svc_builtin_t b;
    memset(&b, 0, sizeof(b));
    b.id = id;
    b.name = id;
    b.restart = PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = autostart;
    b.core = core;
    b.start = f_ok;
    b.stop = f_stop;
    psys_svc_register_builtin(&b);
}

/* Runs one command line (argv[0] is the command) and returns its exit code. Its output (stdout and stderr) lands in out. */
static int run(const char *const *argv, int argc, char *out, size_t cap) {
    nema_obj_t *rd, *wr;
    nema_pipe_create(16384, NEMA_MEM_ANY, OW, &rd, &wr);
    nema_spawn_t sp = {.name = argv[0], .argc = argc, .argv = argv, .stdio = {NULL, wr, wr}};
    nema_pid_t pid = 0;
    nema_err_t r = nema_proc_spawn(&sp, &pid);
    nema_obj_unref(wr);
    if (r != NEMA_OK) {
        nema_obj_unref(rd);
        out[0] = 0;
        return -1000;
    }
    int code = -1;
    nema_proc_wait(pid, &code, 5000);
    size_t n = 0;
    for (;;) {
        int k = nema_obj_read(rd, out + n, cap - n - 1, 0);
        if (k <= 0) break;
        n += (size_t)k;
    }
    out[n] = 0;
    nema_obj_unref(rd);
    return code;
}

#define RUNC(out, ...)                                                      \
    do {                                                                    \
        const char *_av[] = {__VA_ARGS__};                                  \
        run(_av, (int)(sizeof(_av) / sizeof(_av[0])), out, sizeof(out));    \
    } while (0)

TEST(svc_list_shows_every_service_with_its_state_and_source) {
    char out[2048];
    RUNC(out, "svc", "list");
    CHECK(strstr(out, "ID") != NULL && strstr(out, "STATE") != NULL && strstr(out, "SOURCE") != NULL);
    CHECK(strstr(out, "net ") != NULL && strstr(out, "running") != NULL);
    CHECK(strstr(out, "core-ui") != NULL && strstr(out, "built-in, core") != NULL);
    CHECK(strstr(out, "plain") != NULL && strstr(out, "disabled") != NULL);
    CHECK(strstr(out, "SAFE MODE") == NULL);
}

TEST(svc_status_and_errors) {
    char out[2048];
    int code = run((const char *[]){"svc", "status", "plain"}, 3, out, sizeof(out));
    CHECK_EQ(code, 0);
    CHECK(strstr(out, "state     stopped: disabled") != NULL);
    CHECK(strstr(out, "enabled   no") != NULL);
    code = run((const char *[]){"svc", "status", "ghost"}, 3, out, sizeof(out));
    CHECK_EQ(code, 1);
    CHECK(strstr(out, "no service 'ghost'") != NULL);
    code = run((const char *[]){"svc", "stop", "core-ui"}, 3, out, sizeof(out));
    CHECK_EQ(code, 1);
    CHECK(strstr(out, "core-ui is core and cannot be stopped") != NULL);
    code = run((const char *[]){"svc", "bogus"}, 2, out, sizeof(out));
    CHECK_EQ(code, 1);
    CHECK(strstr(out, "usage: svc") != NULL);
}

TEST(svc_start_stop_and_enable_change_the_list) {
    char out[2048];
    CHECK_EQ(run((const char *[]){"svc", "start", "plain"}, 3, out, sizeof(out)), 0);
    RUNC(out, "svc", "list");
    CHECK(strstr(out, "plain") != NULL && strstr(out, "running") != NULL);
    CHECK_EQ(run((const char *[]){"svc", "stop", "plain"}, 3, out, sizeof(out)), 0);
    CHECK_EQ(run((const char *[]){"svc", "enable", "plain"}, 3, out, sizeof(out)), 0);
    psys_svc_info_t in;
    CHECK(psys_svc_find("plain", &in) && in.enabled && in.state == PSYS_SVC_STOPPED);
    CHECK_EQ(run((const char *[]){"svc", "disable", "plain"}, 3, out, sizeof(out)), 0);
    CHECK(psys_svc_find("plain", &in) && !in.enabled);
}

TEST(svc_logs_of_a_service_without_output_is_empty) {
    char out[256];
    int code = run((const char *[]){"svc", "logs", "net"}, 3, out, sizeof(out));
    CHECK_EQ(code, 0);
    CHECK_EQ(strlen(out), 0u);
    code = run((const char *[]){"svc", "logs", "ghost"}, 3, out, sizeof(out));
    CHECK_EQ(code, 1);
}

TEST(safemode_and_boot_report_their_state) {
    char out[2048];
    RUNC(out, "safemode");
    CHECK(strstr(out, "safe mode: off") != NULL);
    RUNC(out, "boot");
    CHECK(strstr(out, "boot target: desktop") != NULL);
    CHECK(strstr(out, "safe mode: no") != NULL);
    CHECK(strstr(out, "boot attempts: 0") != NULL);
    RUNC(out, "boot", "target", "console");
    CHECK(strstr(out, "boot target: console") != NULL);
    RUNC(out, "boot", "target", "nonsense");
    CHECK(strstr(out, "usage: boot target") != NULL);
    RUNC(out, "boot", "target", "desktop");
    int code = run((const char *[]){"safemode", "off"}, 2, out, sizeof(out));
    CHECK_EQ(code, 1); /* no counter on this host */
    CHECK(strstr(out, "cannot be cleared") != NULL);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("test_sh_svc");
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(1024u * 1024u));
    nema_fs_mkdir("/data");
    pal_apps_register_svc();
    pal_apps_register_perf();
    g_now = 0;
    psys_svc_set_clock(fake_clock);
    psys_svc_test_no_task();
    reg("net", true, false);
    reg("core-ui", true, true);
    reg("plain", false, false);
    psys_svc_start_all(false);
    RUN(svc_list_shows_every_service_with_its_state_and_source);
    RUN(svc_status_and_errors);
    RUN(svc_start_stop_and_enable_change_the_list);
    RUN(svc_logs_of_a_service_without_output_is_empty);
    RUN(safemode_and_boot_report_their_state);
    return TEST_DONE();
}
