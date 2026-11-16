/* The service supervisor (system/src/svc*.c): start order and cycles, restart backoff and FAILED with a fake
 * clock, the memory retry, gates, the saved choices in /data/services, safe mode, and installed services (real processes:
 * the ready call, a stop, a crash, the 8 KB log ring, and admission). Built-in services are fakes; the supervisor is driven
 * by psys_svc_step, so no task runs. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "pal/registry.h"
#include "pal/svc.h"
#include "test.h"

#define NSVC 8
#define KB 1024u

static uint64_t g_now; /* microseconds, the fake clock */
static uint64_t fake_clock(void) { return g_now; }
static uint64_t ms(uint64_t t) { return t * 1000u; }
static uint64_t step_at(uint64_t t_ms) {
    g_now = ms(t_ms);
    return psys_svc_step(g_now);
}

/* ---- fake built-in services ---- */
typedef struct {
    int starts, stops;
    int fail_left;   /* starts that return NEMA_ERR_IO before the next success */
    int nomem_left;  /* starts that return NEMA_ERR_NOMEM first */
    bool gate_open;
    int owner_last;  /* on_enabled's last value: -1 none */
} fake_t;
static fake_t F[NSVC];
static int g_order[NSVC * 4], g_nord;

static nema_err_t fake_start(int i) {
    g_order[g_nord++] = i;
    F[i].starts++;
    if (F[i].nomem_left > 0) {
        F[i].nomem_left--;
        return NEMA_ERR_NOMEM;
    }
    if (F[i].fail_left > 0) {
        F[i].fail_left--;
        return NEMA_ERR_IO;
    }
    return NEMA_OK;
}
static void fake_stop(int i) { F[i].stops++; }

#define FAKE(n)                                                                                   \
    static nema_err_t start_##n(void) { return fake_start(n); }                                   \
    static void stop_##n(void) { fake_stop(n); }                                                  \
    static bool gate_##n(void) { return F[n].gate_open; }                                         \
    static void owner_##n(bool on) { F[n].owner_last = on ? 1 : 0; }
FAKE(0) FAKE(1) FAKE(2) FAKE(3) FAKE(4) FAKE(5) FAKE(6) FAKE(7)

static nema_err_t (*const STARTS[NSVC])(void) = {start_0, start_1, start_2, start_3, start_4, start_5, start_6, start_7};
static void (*const STOPS[NSVC])(void) = {stop_0, stop_1, stop_2, stop_3, stop_4, stop_5, stop_6, stop_7};
static bool (*const GATES[NSVC])(void) = {gate_0, gate_1, gate_2, gate_3, gate_4, gate_5, gate_6, gate_7};
static void (*const OWNERS[NSVC])(bool) = {owner_0, owner_1, owner_2, owner_3, owner_4, owner_5, owner_6, owner_7};

static const char *const AFTER_NET[] = {"net", NULL};
static const char *const AFTER_DB[] = {"db", NULL};
static const char *const AFTER_GHOST[] = {"ghost", NULL};
static const char *const AFTER_X[] = {"y", NULL};
static const char *const AFTER_Y[] = {"x", NULL};

/* Registers fake i. opts: flags */
enum { F_GATED = 1, F_CORE = 2, F_NOAUTO = 4, F_OWNED = 8, F_NEVER = 16 };
static void reg(int i, const char *id, const char *const *after, unsigned opts) {
    psys_svc_builtin_t b;
    memset(&b, 0, sizeof(b));
    b.id = id;
    b.name = id;
    b.after = after;
    b.restart = (opts & F_NEVER) ? PSYS_SVC_RESTART_NEVER : PSYS_SVC_RESTART_ON_FAILURE;
    b.autostart = !(opts & F_NOAUTO);
    b.core = (opts & F_CORE) != 0;
    b.start = STARTS[i];
    b.stop = STOPS[i];
    if (opts & F_GATED) {
        b.gate = GATES[i];
        b.waiting_for = "a test condition";
    }
    if (opts & F_OWNED) b.on_enabled = OWNERS[i];
    CHECK_EQ(psys_svc_register_builtin(&b), NEMA_OK);
}

static void reset_all(void) {
    psys_svc_test_reset();
    memset(F, 0, sizeof(F));
    for (int i = 0; i < NSVC; i++) F[i].owner_last = -1;
    g_nord = 0;
    g_now = ms(0);
    nema_fs_unlink("/data/services");
}

static bool state_is(const char *id, psys_svc_state_t s) {
    psys_svc_info_t in;
    return psys_svc_find(id, &in) && in.state == s;
}

/* Whether the saved-choices file contains needle (false when there is no file). */
static bool file_has(const char *needle) {
    nema_obj_t *f;
    if (nema_fs_open("/data/services", NEMA_O_READ, &f) != NEMA_OK) return false;
    char text[256];
    int n = nema_obj_read(f, text, sizeof(text) - 1, 0);
    nema_obj_unref(f);
    if (n <= 0) return false;
    text[n] = 0;
    return strstr(text, needle) != NULL;
}

static bool reason_has(const char *id, const char *needle) {
    psys_svc_info_t in;
    return psys_svc_find(id, &in) && strstr(in.reason, needle) != NULL;
}

static int pos_in_order(int i) {
    for (int k = 0; k < g_nord; k++)
        if (g_order[k] == i) return k;
    return -1;
}

TEST(boot_orders_by_after_and_ignores_unknown_ids) {
    reset_all();
    /* registered out of order: net needs db, web needs net, lonely needs a service that is not there */
    reg(2, "web", AFTER_NET, 0);
    reg(1, "net", AFTER_DB, 0);
    reg(0, "db", NULL, 0);
    reg(3, "lonely", AFTER_GHOST, 0);
    psys_svc_start_all(false);
    CHECK(pos_in_order(0) >= 0 && pos_in_order(1) > pos_in_order(0) && pos_in_order(2) > pos_in_order(1));
    CHECK(pos_in_order(3) >= 0); /* an unknown id in after does not block the service */
    CHECK(state_is("web", PSYS_SVC_RUNNING) && state_is("db", PSYS_SVC_RUNNING) && state_is("lonely", PSYS_SVC_RUNNING));
}

TEST(a_dependency_cycle_fails_its_members_and_starts_the_rest) {
    reset_all();
    reg(0, "x", AFTER_X, 0);
    reg(1, "y", AFTER_Y, 0);
    reg(2, "z", NULL, 0);
    psys_svc_start_all(false);
    CHECK(state_is("x", PSYS_SVC_FAILED) && reason_has("x", "cycle"));
    CHECK(state_is("y", PSYS_SVC_FAILED));
    CHECK(state_is("z", PSYS_SVC_RUNNING));
    CHECK_EQ(F[0].starts, 0);
    CHECK_EQ(F[1].starts, 0);
}

TEST(a_failed_start_retries_with_backoff_then_runs) {
    reset_all();
    reg(0, "flaky", NULL, 0);
    F[0].fail_left = 2;
    psys_svc_start_all(false);
    CHECK_EQ(F[0].starts, 1);
    CHECK(state_is("flaky", PSYS_SVC_STOPPED) && reason_has("flaky", "restart in 1 s"));
    step_at(999);
    CHECK_EQ(F[0].starts, 1); /* the backoff is 1 s */
    step_at(1000);
    CHECK_EQ(F[0].starts, 2);
    CHECK(reason_has("flaky", "restart in 2 s"));
    step_at(2999);
    CHECK_EQ(F[0].starts, 2);
    step_at(3000);
    CHECK_EQ(F[0].starts, 3);
    CHECK(state_is("flaky", PSYS_SVC_RUNNING));
    psys_svc_info_t in;
    psys_svc_find("flaky", &in);
    CHECK_EQ(in.restarts, 2);
}

TEST(five_failures_in_ten_minutes_mark_failed_and_a_manual_start_retries) {
    reset_all();
    reg(0, "doomed", NULL, 0);
    F[0].fail_left = 1000;
    psys_svc_start_all(false); /* failure 1 at 0 s: restart after 1 s */
    step_at(1000);             /* failure 2: backoff 2 s */
    step_at(3000);             /* failure 3: backoff 4 s */
    step_at(7000);             /* failure 4: backoff 8 s */
    CHECK_EQ(F[0].starts, 4);
    CHECK(state_is("doomed", PSYS_SVC_STOPPED));
    step_at(15000); /* failure 5 */
    CHECK_EQ(F[0].starts, 5);
    CHECK(state_is("doomed", PSYS_SVC_FAILED) && reason_has("doomed", "within ten minutes"));
    step_at(3600000); /* an hour later: nothing restarts a FAILED service */
    CHECK_EQ(F[0].starts, 5);
    CHECK_EQ(psys_svc_start("doomed"), NEMA_OK); /* a manual start tries again: this failure is restartable again */
    CHECK_EQ(F[0].starts, 6);
    CHECK(state_is("doomed", PSYS_SVC_STOPPED));
    psys_svc_info_t in;
    psys_svc_find("doomed", &in);
    CHECK_EQ(in.failures, 1); /* the count restarted with the manual start */
}

TEST(failures_outside_the_window_do_not_accumulate) {
    reset_all();
    reg(0, "slow", NULL, 0);
    F[0].fail_left = 4;
    psys_svc_start_all(false); /* failure 1 at 0 s */
    step_at(1000);             /* failure 2 */
    step_at(3000);             /* failure 3 */
    CHECK_EQ(F[0].starts, 3);
    step_at(11 * 60 * 1000 + 3000); /* eleven minutes later: the earlier failures are out of the window */
    CHECK_EQ(F[0].starts, 4);
    CHECK(state_is("slow", PSYS_SVC_STOPPED) && reason_has("slow", "failure 1 of 5"));
}

TEST(memory_refusal_waits_and_retries_every_five_seconds) {
    reset_all();
    reg(0, "big", NULL, 0);
    F[0].nomem_left = 2;
    psys_svc_start_all(false);
    CHECK(state_is("big", PSYS_SVC_WAITING_MEMORY));
    step_at(4999);
    CHECK_EQ(F[0].starts, 1);
    step_at(5000);
    CHECK_EQ(F[0].starts, 2);
    CHECK(state_is("big", PSYS_SVC_WAITING_MEMORY));
    step_at(10000);
    CHECK_EQ(F[0].starts, 3);
    CHECK(state_is("big", PSYS_SVC_RUNNING));
    psys_svc_info_t in;
    psys_svc_find("big", &in);
    CHECK_EQ(in.restarts, 0); /* memory retries are not restarts */
}

TEST(gates_run_a_service_only_while_the_condition_holds) {
    reset_all();
    reg(0, "linkish", NULL, F_GATED);
    psys_svc_start_all(false);
    CHECK_EQ(F[0].starts, 0);
    CHECK(state_is("linkish", PSYS_SVC_STOPPED) && reason_has("linkish", "a test condition"));
    F[0].gate_open = true;
    psys_svc_poke();
    step_at(100);
    CHECK_EQ(F[0].starts, 1);
    CHECK(state_is("linkish", PSYS_SVC_RUNNING));
    F[0].gate_open = false;
    psys_svc_poke();
    step_at(200);
    CHECK_EQ(F[0].stops, 1);
    CHECK(state_is("linkish", PSYS_SVC_STOPPED));
    psys_svc_info_t in;
    psys_svc_find("linkish", &in);
    CHECK(in.enabled); /* a closed gate does not change the choice */
    F[0].gate_open = true;
    psys_svc_poke();
    step_at(300);
    CHECK_EQ(F[0].starts, 2);
}

TEST(a_manual_start_waits_for_a_closed_gate) {
    reset_all();
    reg(0, "linkish", NULL, F_GATED);
    psys_svc_start_all(false);
    CHECK_EQ(psys_svc_start("linkish"), NEMA_OK); /* the choice is recorded; nothing runs yet */
    CHECK_EQ(F[0].starts, 0);
    CHECK(state_is("linkish", PSYS_SVC_STOPPED) && reason_has("linkish", "a test condition"));
    step_at(2000);
    CHECK_EQ(F[0].starts, 0);
    F[0].gate_open = true;
    psys_svc_poke();
    step_at(2100);
    CHECK_EQ(F[0].starts, 1);
    CHECK(state_is("linkish", PSYS_SVC_RUNNING));
}

TEST(a_manual_stop_is_remembered_across_a_boot) {
    reset_all();
    reg(0, "web", NULL, 0);
    psys_svc_start_all(false);
    CHECK(state_is("web", PSYS_SVC_RUNNING));
    CHECK_EQ(psys_svc_stop("web"), NEMA_OK);
    CHECK_EQ(F[0].stops, 1);
    CHECK(state_is("web", PSYS_SVC_STOPPED) && reason_has("web", "stopped by hand"));
    /* the file keeps the line, and the next boot honours it */
    CHECK(file_has("web=disabled\n"));

    psys_svc_test_reset(); /* a new boot: the table is rebuilt, the file stays */
    reg(0, "web", NULL, 0);
    psys_svc_start_all(false);
    CHECK(state_is("web", PSYS_SVC_STOPPED) && reason_has("web", "disabled"));
    CHECK_EQ(F[0].starts, 1); /* the old boot's start only */
    CHECK_EQ(psys_svc_start("web"), NEMA_OK);
    CHECK(state_is("web", PSYS_SVC_RUNNING));
    CHECK_EQ(psys_svc_set_enabled("web", false), NEMA_OK);
    CHECK_EQ(psys_svc_set_enabled("nope", true), NEMA_ERR_NOENT);
}

TEST(radio_style_services_keep_their_choice_with_their_owner) {
    reset_all();
    reg(0, "radio", NULL, F_OWNED);
    psys_svc_start_all(false);
    CHECK(state_is("radio", PSYS_SVC_RUNNING));
    CHECK_EQ(psys_svc_stop("radio"), NEMA_OK);
    CHECK_EQ(F[0].owner_last, 0); /* the owner heard "off" */
    CHECK(!file_has("radio=")); /* its choice is in the radio settings, not in this file */
    CHECK_EQ(psys_svc_start("radio"), NEMA_OK);
    CHECK_EQ(F[0].owner_last, 1);
}

TEST(safe_mode_starts_only_core_services) {
    reset_all();
    reg(0, "core-ui", NULL, F_CORE);
    reg(1, "plain", NULL, 0);
    psys_svc_start_all(true);
    CHECK(psys_svc_safe_mode());
    CHECK(state_is("core-ui", PSYS_SVC_RUNNING));
    CHECK(state_is("plain", PSYS_SVC_STOPPED) && reason_has("plain", "safe mode"));
    CHECK_EQ(F[1].starts, 0);
    /* a manual start is allowed; its failure is not retried automatically in safe mode */
    F[1].fail_left = 1;
    CHECK_EQ(psys_svc_start("plain"), NEMA_OK);
    CHECK_EQ(F[1].starts, 1);
    step_at(60 * 1000);
    CHECK_EQ(F[1].starts, 1);
    CHECK(state_is("plain", PSYS_SVC_STOPPED));
    CHECK_EQ(psys_svc_stop("core-ui"), NEMA_ERR_PERM); /* core cannot be stopped */
}

/* ---- installed services: real processes ---- */

static nema_owner_t OW;
static volatile int g_lines_written;

/* logger: 1300 lines of 7 bytes (9100 bytes), then exits 0. Writes block when the pipe is full: the test steps the
 * supervisor, which drains it. */
static int app_logger(int argc, char **argv) {
    (void)argc, (void)argv;
    char line[16];
    for (int i = 0; i < 1300; i++) {
        int n = snprintf(line, sizeof(line), "L%05d\n", i);
        nema_write(1, line, (size_t)n);
    }
    return 0;
}

/* ready: calls pal_svc_ready at once, then runs until it is stopped. */
static int app_ready(int argc, char **argv) {
    (void)argc, (void)argv;
    (void)psys_svc_ready_self();
    nema_sleep_ms(20000);
    return 0;
}

static int app_crash(int argc, char **argv) {
    (void)argc, (void)argv;
    nema_write(1, "about to fail\n", 14);
    return 3;
}

static pal_papp_t svc_pkg(const char *id) {
    pal_papp_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id, sizeof(a.id), "%s", id);
    snprintf(a.name, sizeof(a.name), "%s", id);
    snprintf(a.version, sizeof(a.version), "0.1");
    a.api = 2;
    a.kind = PAL_KIND_SERVICE;
    return a;
}

/* Installs a package the way papp_pkg.c does: the registry entry first, then the command (its name is the id). The
 * command table lives here, so the commands outlive the test. */
static nema_app_t CMDS[NSVC * 2];
static char CMD_IDS[NSVC * 2][PSYS_SVC_ID_LEN];
static int ncmds;

static void install(const char *id, int (*main_fn)(int, char **)) {
    pal_papp_t a = svc_pkg(id);
    char path[64];
    snprintf(path, sizeof(path), "/apps/%s.papp", id);
    CHECK_EQ(psys_reg_add_installed(&a, path), NEMA_OK);
    int k = ncmds++;
    snprintf(CMD_IDS[k], sizeof(CMD_IDS[k]), "%s", id);
    CMDS[k].name = CMD_IDS[k];
    CMDS[k].help = "test service";
    CMDS[k].main = main_fn;
    CMDS[k].stack = 8192;
    CMDS[k].flags = NEMA_APP_HIDDEN;
    CHECK_EQ(nema_app_register(&CMDS[k]), NEMA_OK);
}

/* The byte at absolute offset off of the logger's output: line i is "L%05d\n", seven bytes. */
static char expected_byte(uint64_t off) {
    uint64_t line = off / 7;
    int p = (int)(off % 7);
    if (p == 0) return 'L';
    if (p == 6) return '\n';
    uint64_t divisor = 1;
    for (int d = p; d < 5; d++) divisor *= 10;
    return (char)('0' + (int)((line / divisor) % 10));
}

/* Steps the supervisor at the current fake time (draining pipes, reaping, timers) until the service is out of STARTING and
 * no process is running. The clock does not move: callers move it. */
static void settle(const char *id, int max_rounds) {
    for (int i = 0; i < max_rounds; i++) {
        psys_svc_info_t in;
        (void)psys_svc_step(g_now);
        if (psys_svc_find(id, &in) && in.state != PSYS_SVC_STARTING && in.pid == 0) return;
        nema_sleep_ms(2);
    }
}

TEST(installed_service_logs_into_a_ring_that_keeps_the_newest_bytes) {
    reset_all();
    install("logger", app_logger);
    CHECK_EQ(psys_svc_start("logger"), NEMA_OK);
    settle("logger", 2000);
    psys_svc_info_t in;
    CHECK(psys_svc_find("logger", &in));
    CHECK(in.installed);
    CHECK(in.state == PSYS_SVC_STOPPED && strstr(in.reason, "exited (code 0)") != NULL);
    CHECK_EQ(in.log_total, 9100u);
    /* the ring keeps 8 KB: a cursor at 0 gets the newest 8192 bytes, each at its absolute offset */
    uint64_t cur = 0;
    char buf[300];
    size_t got = 0, want_off = in.log_total - PSYS_SVC_LOG_BYTES;
    int bad = 0;
    for (;;) {
        int n = psys_svc_log_read("logger", &cur, buf, sizeof(buf));
        if (n <= 0) break;
        for (int k = 0; k < n; k++, got++)
            if (buf[k] != expected_byte(want_off + got)) bad++;
    }
    CHECK_EQ(got, PSYS_SVC_LOG_BYTES);
    CHECK_EQ(bad, 0);
    CHECK_EQ(psys_svc_log_read("logger", &cur, buf, sizeof(buf)), 0); /* nothing new */
    CHECK_EQ(psys_svc_log_wait("logger", in.log_total, 10), NEMA_ERR_TIMEOUT);
    CHECK_EQ(psys_svc_log_read("nobody", &cur, buf, sizeof(buf)), NEMA_ERR_NOENT);
}

TEST(a_service_that_calls_ready_is_running_and_a_stop_ends_it) {
    reset_all();
    install("ready", app_ready);
    CHECK_EQ(psys_svc_start("ready"), NEMA_OK);
    step_at(10);
    /* the process calls pal_svc_ready on its own thread: wait for the flag, then the supervisor marks it running */
    for (int i = 0; i < 2000 && !state_is("ready", PSYS_SVC_RUNNING); i++) {
        step_at(10 + (uint64_t)i);
        nema_sleep_ms(1);
    }
    CHECK(state_is("ready", PSYS_SVC_RUNNING));
    psys_svc_info_t in;
    psys_svc_find("ready", &in);
    CHECK(in.pid != 0);
    CHECK_EQ(psys_svc_stop("ready"), NEMA_OK);
    CHECK(state_is("ready", PSYS_SVC_STOPPED) && reason_has("ready", "stopped by hand"));
    psys_svc_find("ready", &in);
    CHECK_EQ(in.pid, 0);
    CHECK_EQ(psys_svc_ready_self(), NEMA_ERR_NOENT); /* the test thread is not a service */
}

TEST(a_crashing_service_is_restarted_then_marked_failed) {
    reset_all();
    install("crasher", app_crash);
    CHECK_EQ(psys_svc_start("crasher"), NEMA_OK);
    uint64_t t = 0;
    for (int run = 0; run < 5; run++) {
        settle("crasher", 2000);
        psys_svc_info_t in;
        psys_svc_find("crasher", &in);
        if (run < 4) {
            CHECK(in.state == PSYS_SVC_STOPPED);
            CHECK_EQ((int)in.failures, run + 1);
            t += (uint64_t)(1u << run) * 1000u; /* the backoff: 1, 2, 4, 8 s */
            step_at(t);
        } else {
            CHECK(in.state == PSYS_SVC_FAILED);
            CHECK(strstr(in.reason, "exited with code 3") != NULL);
        }
    }
    psys_svc_info_t in;
    psys_svc_find("crasher", &in);
    CHECK_EQ(in.restarts, 4);
}

/* Admission (registry.c): at most PSYS_REG_MAX_WASM installed apps run at once, services included. The fifth is refused
 * with the reason, waits, and starts when one of the four stops. */
TEST(admission_refuses_a_fifth_installed_app_and_retries_when_one_stops) {
    reset_all();
    for (int i = 0; i < 5; i++) {
        char id[8];
        snprintf(id, sizeof(id), "s%d", i);
        install(id, app_ready);
    }
    for (int i = 0; i < 4; i++) {
        char id[8];
        snprintf(id, sizeof(id), "s%d", i);
        CHECK_EQ(psys_svc_start(id), NEMA_OK);
    }
    /* let the four reach ready */
    for (int i = 0; i < 2000; i++) {
        step_at(100 + (uint64_t)i);
        bool all = true;
        for (int k = 0; k < 4; k++) {
            char id[8];
            snprintf(id, sizeof(id), "s%d", k);
            all = all && state_is(id, PSYS_SVC_RUNNING);
        }
        if (all) break;
        nema_sleep_ms(1);
    }
    CHECK(state_is("s3", PSYS_SVC_RUNNING));
    CHECK_EQ(psys_svc_start("s4"), NEMA_OK); /* a waiting state is not an error for the caller */
    CHECK(state_is("s4", PSYS_SVC_WAITING_MEMORY) && reason_has("s4", "already running"));
    CHECK_EQ(psys_svc_stop("s0"), NEMA_OK);
    step_at(100000); /* the retry is due 5 s after the refusal; this step is well past it */
    CHECK(state_is("s4", PSYS_SVC_STARTING) || state_is("s4", PSYS_SVC_RUNNING));
    for (int i = 1; i < 5; i++) {
        char id[8];
        snprintf(id, sizeof(id), "s%d", i);
        (void)psys_svc_stop(id);
    }
}

TEST(list_counts_the_builtin_services) {
    reset_all();
    reg(0, "net", NULL, 0);
    psys_svc_start_all(false);
    size_t n = psys_svc_count();
    CHECK(n >= 1);
    psys_svc_info_t in;
    CHECK(psys_svc_info(0, &in) && strcmp(in.id, "net") == 0 && in.core == false);
    CHECK(!psys_svc_info(n, &in));
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("test_svc");
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(1024u * 1024u));
    nema_fs_mkdir("/data");
    nema_fs_mkdir("/apps");
    psys_svc_set_clock(fake_clock);
    psys_svc_test_no_task();
    RUN(boot_orders_by_after_and_ignores_unknown_ids);
    RUN(a_dependency_cycle_fails_its_members_and_starts_the_rest);
    RUN(a_failed_start_retries_with_backoff_then_runs);
    RUN(five_failures_in_ten_minutes_mark_failed_and_a_manual_start_retries);
    RUN(failures_outside_the_window_do_not_accumulate);
    RUN(memory_refusal_waits_and_retries_every_five_seconds);
    RUN(gates_run_a_service_only_while_the_condition_holds);
    RUN(a_manual_start_waits_for_a_closed_gate);
    RUN(a_manual_stop_is_remembered_across_a_boot);
    RUN(radio_style_services_keep_their_choice_with_their_owner);
    RUN(safe_mode_starts_only_core_services);
    RUN(installed_service_logs_into_a_ring_that_keeps_the_newest_bytes);
    RUN(a_service_that_calls_ready_is_running_and_a_stop_ends_it);
    RUN(a_crashing_service_is_restarted_then_marked_failed);
    RUN(admission_refuses_a_fifth_installed_app_and_retries_when_one_stops);
    RUN(list_counts_the_builtin_services);
    return TEST_DONE();
}
