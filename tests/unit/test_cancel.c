/* Safe cancellation: TERM with a grace period, KILL of native apps, the kill hook, and deferred writes. */
#include <string.h>

#include "nema/nema.h"
#include "test.h"

static nema_owner_t OW;
static volatile int g_hook_ran;
static volatile int g_res[4]; /* results of app_defer (a killed process cannot write them out) */
static uint8_t g_blob[65536];

static void on_kill(void *ctx) { *(volatile int *)ctx = 1; }

/* blocked: waits in read(); a cancel returns CANCELED, an interrupt INTR; both end the app */
static int app_blocked(int argc, char **argv) {
    char b[4];
    for (;;) {
        int r = nema_read(0, b, sizeof(b));
        if (r < 0) return r == NEMA_ERR_CANCELED ? 3 : 4;
    }
}

/* polite: returns 130 on TERM (the interrupt is cooperative) */
static int app_polite(int argc, char **argv) {
    char b[4];
    int r = nema_read(0, b, sizeof(b));
    (void)r;
    return nema_interrupted() ? 130 : 0;
}

/* poller: waits in nema_poll with a long timeout; a kill ends the wait at once */
static int app_poller(int argc, char **argv) {
    nema_pollfd_t p[1] = {{0, NEMA_POLL_IN, 0}};
    int r = nema_poll(p, 1, 10000);
    return r == NEMA_ERR_CANCELED ? 3 : 4;
}

/* busy: a loop that only calls the API; once killed, the first call returns CANCELED */
static int app_busy(int argc, char **argv) {
    for (;;) {
        if (nema_sleep_ms(0) == NEMA_ERR_CANCELED) return 3;
    }
}

/* deaf: ignores every error and keeps calling read(): the kernel ends it after NEMA_CANCEL_STRIKES calls */
static int app_deaf(int argc, char **argv) {
    char b[4];
    for (;;) (void)nema_read(0, b, sizeof(b));
}

/* parent: starts a child in a group of its own (like a background job) and waits for stdin */
static int app_parent(int argc, char **argv) {
    nema_spawn_t sp = {.name = "cancel-blocked", .pgid = 0, .flags = NEMA_SPAWN_DETACH};
    nema_pid_t kid;
    (void)nema_proc_spawn(&sp, &kid);
    char b[4];
    for (;;) {
        int r = nema_read(0, b, sizeof(b));
        if (r < 0) return 5;
    }
}

/* stubborn: ignores TERM (its calls return INTR at once, so it spins) until KILL */
static int app_stubborn(int argc, char **argv) {
    while (nema_sleep_ms(10) != NEMA_ERR_CANCELED) {
    }
    return 0;
}

/* hooked: registers a kill hook, then blocks */
static int app_hooked(int argc, char **argv) {
    nema_proc_set_kill_hook(on_kill, (void *)&g_hook_ran);
    char b[4];
    nema_read(0, b, sizeof(b));
    return 0;
}

/* defer: a write made inside nema_cancel_defer() is never cut short. A kill that arrives during the defer does not
 * interrupt the sleep or the write; it applies at the first call after the outermost allow. */
static int app_defer(int argc, char **argv) {
    nema_cancel_defer();
    nema_cancel_defer();
    nema_write(1, "d", 1); /* tells the parent the defer is in effect, so its kill lands inside it */
    g_res[0] = nema_sleep_ms(150);
    nema_cancel_allow();
    g_res[1] = nema_sleep_ms(50); /* still deferred: nesting */
    int fd = nema_open("/defer.bin", NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC);
    g_res[2] = fd >= 0 ? nema_write(fd, g_blob, sizeof(g_blob)) : fd;
    nema_close(fd);
    nema_cancel_allow();
    g_res[3] = nema_sleep_ms(10); /* the kill applies now */
    return 0;
}

static const nema_app_t APPS[] = {
    {"cancel-blocked", "blocked in read", app_blocked, 0, 0},
    {"cancel-polite", "returns on TERM", app_polite, 0, 0},
    {"cancel-poller", "blocked in poll", app_poller, 0, 0},
    {"cancel-busy", "busy loop on the API", app_busy, 0, 0},
    {"cancel-stubborn", "ignores TERM", app_stubborn, 0, 0},
    {"cancel-parent", "starts a job in its own group", app_parent, 0, 0},
    {"deaf", "ignores every error from read", app_deaf, 0, 0},
    {"cancel-hooked", "kill hook", app_hooked, 0, 0},
    {"cancel-defer", "write under defer", app_defer, 0, 0},
};

static size_t proc_bytes(void) {
    size_t total = 0;
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        nema_owner_stat_t s;
        if (!nema_mem_owner_stat((nema_owner_t)(NEMA_OWNER_PROC_BASE + i), &s)) continue;
        total += s.used[NEMA_MEM_INTERNAL] + s.used[NEMA_MEM_DMA] + s.used[NEMA_MEM_EXTERNAL] + s.stack;
    }
    return total;
}

/* Task stacks are un-accounted just after a process is reaped, so allow a moment before comparing. */
static bool settle(size_t base) {
    for (int i = 0; i < 100; i++) {
        if (proc_bytes() == base) return true;
        nema_port_sleep_ms(5);
    }
    return proc_bytes() == base;
}

static uint64_t ms_since(uint64_t t0) { return (nema_port_time_us() - t0) / 1000u; }

/* Spawns `name` with a pipe on stdin; returns the pid and, through w_out, the write end (closing it ends input). */
static nema_pid_t start(const char *name, nema_obj_t **w_out) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = name, .stdio = {r, NULL, NULL}};
    nema_pid_t pid = -1;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_obj_unref(r); /* the process holds its own reference */
    *w_out = w;
    return pid;
}

TEST(kill_ends_a_process_blocked_in_a_pipe_read) {
    size_t base = proc_bytes();
    nema_obj_t *w;
    nema_pid_t pid = start("cancel-blocked", &w);
    nema_port_sleep_ms(50);
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    uint64_t t0 = nema_port_time_us();
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_OK);
    CHECK(ms_since(t0) < 200); /* returned at once, not at a timeout */
    CHECK_EQ(code, 137);       /* killed: reported as such */
    nema_obj_unref(w);
    CHECK(settle(base));
}

TEST(kill_ends_a_process_blocked_in_poll) {
    size_t base = proc_bytes();
    nema_obj_t *w;
    nema_pid_t pid = start("cancel-poller", &w);
    nema_port_sleep_ms(50);
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    uint64_t t0 = nema_port_time_us();
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_OK);
    CHECK(ms_since(t0) < 200);
    CHECK_EQ(code, 137);
    nema_obj_unref(w);
    CHECK(settle(base));
}

TEST(kill_stops_a_busy_loop_that_calls_the_api) {
    size_t base = proc_bytes();
    nema_spawn_t sp = {.name = "cancel-busy"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(50);
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_OK);
    CHECK_EQ(code, 137);
    CHECK(settle(base));
}

TEST(kill_ends_a_process_that_ignores_cancellation) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = "deaf", .stdio = {r, NULL, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(60);
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1500) == NEMA_OK);
    CHECK_EQ(code, 137);
    nema_obj_unref(w);
    nema_obj_unref(r);
}

TEST(signalling_a_tree_reaches_jobs_in_their_own_group) {
    size_t base_tasks = nema_task_count();
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = "cancel-parent", .stdio = {r, r, r}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(80);
    int n = nema_proc_signal_tree(pid, NEMA_SIG_HUP | NEMA_SIG_KILL);
    CHECK(n >= 2); /* the parent and the job it started */
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1500) == NEMA_OK);
    nema_port_sleep_ms(100);
    CHECK_EQ(nema_proc_signal_tree(pid, NEMA_SIG_KILL), 0); /* the parent is gone */
    CHECK_EQ(nema_task_count(), base_tasks);                /* and so is the job it started in its own group */
    nema_obj_unref(w);
    nema_obj_unref(r);
}

TEST(kill_runs_the_registered_hook_once) {
    size_t base = proc_bytes();
    g_hook_ran = 0;
    nema_obj_t *w;
    nema_pid_t pid = start("cancel-hooked", &w);
    nema_port_sleep_ms(50);
    CHECK_EQ(g_hook_ran, 0);
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_OK);
    CHECK_EQ(g_hook_ran, 1);
    nema_obj_unref(w);
    CHECK(settle(base));
}

TEST(term_lets_a_polite_app_return_without_a_kill) {
    nema_obj_t *w;
    nema_pid_t pid = start("cancel-polite", &w);
    nema_port_sleep_ms(50);
    CHECK(nema_proc_signal(pid, NEMA_SIG_TERM) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_OK);
    CHECK_EQ(code, 130);
    nema_obj_unref(w);
}

TEST(term_is_killed_after_the_grace_period) {
    size_t base = proc_bytes();
    nema_spawn_t sp = {.name = "cancel-stubborn"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(30);
    uint64_t t0 = nema_port_time_us();
    CHECK(nema_proc_signal(pid, NEMA_SIG_TERM) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_ERR_TIMEOUT); /* still inside the grace period */
    CHECK(nema_proc_wait(pid, &code, 6000) == NEMA_OK);
    uint64_t took = ms_since(t0);
    CHECK(took >= NEMA_TERM_GRACE_MS - 100 && took < NEMA_TERM_GRACE_MS + 1500);
    CHECK_EQ(code, 137);
    CHECK(settle(base));
    /* the reaper task leaves once nothing is pending */
    for (int i = 0; i < 100 && nema_task_count() != 0; i++) nema_port_sleep_ms(10);
    CHECK_EQ(nema_task_count(), 0);
}

TEST(a_kill_during_a_deferred_write_applies_after_it) {
    size_t base = proc_bytes();
    nema_obj_t *r, *w;
    nema_pipe_create(64, NEMA_MEM_ANY, OW, &r, &w);
    nema_obj_t *out_r, *out_w;
    nema_pipe_create(64, NEMA_MEM_ANY, OW, &out_r, &out_w);
    nema_spawn_t sp = {.name = "cancel-defer", .stdio = {r, out_w, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_obj_unref(r);
    nema_obj_unref(out_w);
    char ready = 0;
    CHECK_EQ(nema_obj_read(out_r, &ready, 1, 2000), 1);
    CHECK_EQ(ready, 'd');
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK); /* lands inside the deferred sleep */
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    /* the sleeps inside the defer ran in full, the write completed, and the kill cut the first call after it */
    CHECK_EQ(g_res[0], NEMA_OK);
    CHECK_EQ(g_res[1], NEMA_OK);
    CHECK_EQ(g_res[2], 65536);
    CHECK_EQ(g_res[3], NEMA_ERR_CANCELED);
    nema_stat_t st;
    CHECK(nema_fs_stat("/defer.bin", &st) == NEMA_OK);
    CHECK_EQ(st.size, 65536);
    CHECK_EQ(code, 137);
    CHECK(settle(base));
    nema_obj_unref(w);
    nema_obj_unref(out_r);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("canceltest");
    void *fs = nema_ramfs_create(1024u * 1024u);
    nema_vfs_mount("/", nema_ramfs_ops(), fs);
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    for (size_t i = 0; i < sizeof(g_blob); i++) g_blob[i] = (uint8_t)i;
    RUN(kill_ends_a_process_blocked_in_a_pipe_read);
    RUN(kill_ends_a_process_blocked_in_poll);
    RUN(kill_stops_a_busy_loop_that_calls_the_api);
    RUN(kill_ends_a_process_that_ignores_cancellation);
    RUN(signalling_a_tree_reaches_jobs_in_their_own_group);
    RUN(kill_runs_the_registered_hook_once);
    RUN(term_lets_a_polite_app_return_without_a_kill);
    RUN(term_is_killed_after_the_grace_period);
    RUN(a_kill_during_a_deferred_write_applies_after_it);
    return TEST_DONE();
}
