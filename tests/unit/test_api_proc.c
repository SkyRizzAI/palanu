/* The proc, time and log modules of the palanu API, end to end: the hand-encoded apps of tests/unit/wasm_fixtures_core.h
 * start children, read the environment and the clock, and write to the system log. The glue checks the capabilities:
 * an app without proc.spawn cannot start a child. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "pal/time.h"
#include "test.h"
#include "wasm_fixtures_core.h"

static nema_owner_t OW;

static int run_mod(const uint8_t *mod, size_t n, const char *id, uint32_t perms, int argc, char **argv) {
    uint8_t *img = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, n);
    if (!img) return 125;
    memcpy(img, mod, n);
    pal_papp_t app;
    memset(&app, 0, sizeof(app));
    snprintf(app.id, sizeof(app.id), "%s", id);
    app.api = 2;
    app.perms = perms;
    app.mem_kb = 64;
    app.stack_kb = 16;
    app.payload = img;
    app.payload_len = n;
    return pal_wasm_run(&app, img, argc, argv);
}

#define PERM_PROC 0x20u /* PAL_PERM_PROC */
#define APP_FN(fn, mod, id, perms) \
    static int fn(int argc, char **argv) { return run_mod(mod, sizeof(mod), id, perms, argc, argv); }

APP_FN(a_env, WASM_ENV, "api-env", 0)
APP_FN(a_time, WASM_TIME, "api-time", 0)
APP_FN(a_log, WASM_LOG, "api-log", 0)
APP_FN(a_spawn, WASM_SPAWN, "api-spawn", PERM_PROC)
APP_FN(a_spawn_noperm, WASM_SPAWN, "api-spawn-noperm", 0)
APP_FN(a_spawn_term, WASM_SPAWN_TERM, "api-spawn-term", PERM_PROC)
APP_FN(a_spawn_kill, WASM_SPAWN_KILL, "api-spawn-kill", PERM_PROC)
APP_FN(a_guard, WASM_GUARD, "api-guard", PERM_PROC)
APP_FN(a_orphan, WASM_ORPHAN, "api-orphan", PERM_PROC)
APP_FN(a_kid, WASM_KID, "api-kid", 0)
APP_FN(a_sleeper, WASM_SLEEPER, "api-sleeper", 0)

static const nema_app_t APPS[] = {
    {"api-env", "", a_env, 0, 0},
    {"api-time", "", a_time, 0, 0},
    {"api-log", "", a_log, 0, 0},
    {"api-spawn", "", a_spawn, 0, 0},
    {"api-spawn-noperm", "", a_spawn_noperm, 0, 0},
    {"api-spawn-term", "", a_spawn_term, 0, 0},
    {"api-spawn-kill", "", a_spawn_kill, 0, 0},
    {"api-guard", "", a_guard, 0, 0},
    {"api-orphan", "", a_orphan, 0, 0},
    {"api-kid", "", a_kid, 0, 0},
    {"api-sleeper", "", a_sleeper, 0, 0},
};

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void make_pipe(nema_obj_t **rd, nema_obj_t **wr) { nema_pipe_create(4096, NEMA_MEM_ANY, OW, rd, wr); }

/* Starts a registered app with the given stdout, stdin from the null device, and an optional working directory. */
static nema_pid_t start(const char *name, nema_obj_t *out, const char *cwd) {
    nema_spawn_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.name = name;
    sp.stdio[1] = out;
    sp.cwd = cwd;
    nema_pid_t pid = 0;
    return nema_proc_spawn(&sp, &pid) == NEMA_OK ? pid : 0;
}

static size_t drain(nema_obj_t *rd, uint8_t *buf, size_t cap) {
    size_t n = 0;
    while (n < cap) {
        int k = nema_obj_read(rd, buf + n, cap - n, 1000);
        if (k <= 0) break;
        n += (size_t)k;
    }
    return n;
}

/* Runs an app with an output pipe and returns its status; out receives what it wrote, n how much. */
static int run_app(const char *name, const char *cwd, uint8_t *out, size_t cap, size_t *n) {
    nema_obj_t *rd, *wr;
    make_pipe(&rd, &wr);
    nema_pid_t pid = start(name, wr, cwd);
    nema_obj_unref(wr);
    int code = -1;
    if (!pid || nema_proc_wait(pid, &code, 5000) != NEMA_OK) code = -1000;
    *n = drain(rd, out, cap);
    nema_obj_unref(rd);
    return code;
}

/* ---- environment ---- */

TEST(getenv_reads_the_fixed_environment_and_the_working_directory) {
    uint8_t out[20 + 160];
    size_t n = 0;
    CHECK_EQ(run_app("api-env", "/home", out, sizeof(out), &n), 0);
    CHECK_EQ(n, sizeof(out));
    CHECK_EQ((int32_t)le32(out + 0), 5);   /* HOME = /home */
    CHECK_EQ((int32_t)le32(out + 4), 5);   /* PWD = the working directory, /home */
    CHECK_EQ((int32_t)le32(out + 8), -3);  /* NOPE is not set: a negative error (not found) */
    CHECK_EQ((int32_t)le32(out + 12), 6);  /* USER = palanu */
    CHECK_EQ((int32_t)le32(out + 16), 14); /* TERM = xterm-256color */
    CHECK(strcmp((char *)out + 20 + 0, "/home") == 0);
    CHECK(strcmp((char *)out + 20 + 32, "/home") == 0);
    CHECK(strcmp((char *)out + 20 + 64, "") == 0);
    CHECK(strcmp((char *)out + 20 + 96, "palanu") == 0);
    CHECK(strcmp((char *)out + 20 + 128, "xterm-256color") == 0);
}

/* ---- clock ---- */

TEST(unix_time_and_localtime_follow_the_clock_and_the_timezone) {
    uint8_t out[8 + 4 + 36];
    pal_time_set(1000000000u); /* 2001-09-09 01:46:40 UTC, a Sunday, day 252 of the year */
    pal_time_set_tz_minutes(0);
    size_t n = 0;
    CHECK_EQ(run_app("api-time", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ(n, sizeof(out));
    int64_t unix_now = (int64_t)le32(out) | (int64_t)le32(out + 4) << 32;
    CHECK(unix_now >= 1000000000 && unix_now < 1000000060); /* the clock runs from the set value */
    CHECK_EQ((int32_t)le32(out + 8), 0); /* pal_localtime returned 0 */
    const uint8_t *tm = out + 12;
    CHECK_EQ((int32_t)le32(tm + 0), 40);  /* sec */
    CHECK_EQ((int32_t)le32(tm + 4), 46);  /* min */
    CHECK_EQ((int32_t)le32(tm + 8), 1);   /* hour, UTC */
    CHECK_EQ((int32_t)le32(tm + 12), 9);  /* mday */
    CHECK_EQ((int32_t)le32(tm + 16), 8);  /* mon: September, 0-based */
    CHECK_EQ((int32_t)le32(tm + 20), 101); /* year since 1900 */
    CHECK_EQ((int32_t)le32(tm + 24), 0);  /* wday: Sunday */
    CHECK_EQ((int32_t)le32(tm + 28), 251); /* yday: 0-based, September 9 */
    CHECK_EQ((int32_t)le32(tm + 32), 0);  /* isdst */

    pal_time_set_tz_minutes(7 * 60); /* the board's default, WIB */
    CHECK_EQ(run_app("api-time", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ((int32_t)le32(out + 12 + 8), 8); /* 08:46:40 local */
    CHECK_EQ((int32_t)le32(out + 12 + 12), 9);
    pal_time_set_tz_minutes(0);
}

/* ---- log ---- */

TEST(log_writes_a_line_tagged_with_the_app_id_and_refuses_an_unknown_level) {
    uint8_t out[8];
    size_t n = 0;
    CHECK_EQ(run_app("api-log", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ(n, 8u);
    CHECK_EQ((int32_t)le32(out), 0);
    CHECK_EQ((int32_t)le32(out + 4), -2); /* level 9: invalid argument */
    static char snap[4096];
    size_t got = nema_log_snapshot(snap, sizeof(snap) - 1);
    snap[got] = 0;
    CHECK(strstr(snap, "api-log: hello from app") != NULL);
}

/* ---- child processes ---- */

TEST(spawn_passes_the_arguments_and_reports_the_child_status) {
    uint8_t out[1 + 8 + 4];
    size_t n = 0;
    CHECK_EQ(run_app("api-spawn", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ(n, sizeof(out));
    CHECK_EQ(out[0], '3');                    /* the child saw argv[0] plus the two words "a" and "b" */
    CHECK(le32(out + 1) > 0);                 /* the child handle (its pid) */
    CHECK_EQ((int32_t)le32(out + 5), 1);      /* pal_child_wait returned 1 */
    CHECK_EQ((int32_t)le32(out + 9), 7);      /* the child's status */
}

TEST(spawn_without_the_proc_spawn_permission_is_refused) {
    uint8_t out[8];
    size_t n = 0;
    CHECK_EQ(run_app("api-spawn-noperm", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ((int32_t)le32(out), -9);         /* pal_spawn: permission denied */
    CHECK_EQ((int32_t)le32(out + 4), -3);     /* the child wait finds no child */
}

TEST(a_child_is_stopped_by_term_and_by_kill) {
    uint8_t out[12 + 4];
    size_t n = 0;
    CHECK_EQ(run_app("api-spawn-term", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ(n, sizeof(out));
    CHECK(le32(out) > 0);
    CHECK_EQ((int32_t)le32(out + 4), 0);      /* pal_signal accepted */
    CHECK_EQ((int32_t)le32(out + 8), 1);      /* pal_child_wait returned 1 */
    CHECK_EQ((int32_t)le32(out + 12), 130);   /* the sleeper returned at once: interrupted */

    CHECK_EQ(run_app("api-spawn-kill", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ((int32_t)le32(out + 12), 137);   /* killed */
}

TEST(an_app_cannot_wait_for_or_signal_a_process_it_did_not_start) {
    uint8_t out[8];
    size_t n = 0;
    CHECK_EQ(run_app("api-guard", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ((int32_t)le32(out), -3);         /* pal_child_wait: not a child */
    CHECK_EQ((int32_t)le32(out + 4), -3);     /* pal_signal: not a child */
}

TEST(a_child_left_behind_is_reaped_when_its_app_ends) {
    uint8_t out[8];
    size_t n = 0;
    CHECK_EQ(run_app("api-orphan", NULL, out, sizeof(out), &n), 0);
    CHECK_EQ(n, 5u);                          /* the child's one digit, then the pid */
    CHECK_EQ(out[0], '3');
    nema_pid_t pid = (nema_pid_t)le32(out + 1);
    nema_port_sleep_ms(100);
    CHECK(nema_proc_get(pid) == NULL);        /* finished and nobody waits: the slot is free again */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("apiproc");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    RUN(getenv_reads_the_fixed_environment_and_the_working_directory);
    RUN(unix_time_and_localtime_follow_the_clock_and_the_timezone);
    RUN(log_writes_a_line_tagged_with_the_app_id_and_refuses_an_unknown_level);
    RUN(spawn_passes_the_arguments_and_reports_the_child_status);
    RUN(spawn_without_the_proc_spawn_permission_is_refused);
    RUN(a_child_is_stopped_by_term_and_by_kill);
    RUN(an_app_cannot_wait_for_or_signal_a_process_it_did_not_start);
    RUN(a_child_left_behind_is_reaped_when_its_app_ends);
    return TEST_DONE();
}
