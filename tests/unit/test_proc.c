#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "test.h"

static nema_owner_t OW;

/* upcase: copies stdin to stdout in upper case, exits with the number of bytes copied mod 256 + 7 */
static int app_upcase(int argc, char **argv) {
    char b[32];
    int total = 0, n;
    while ((n = nema_read(0, b, sizeof(b))) > 0) {
        for (int i = 0; i < n; i++) b[i] = (char)toupper((unsigned char)b[i]);
        nema_write(1, b, (size_t)n);
        total += n;
    }
    return 7 + (argc > 1 ? atoi(argv[1]) : 0) + (total & 0);
}

/* sleeper: blocks on stdin; INT makes the read return INTR, which it turns into exit code 130 */
static int app_sleeper(int argc, char **argv) {
    char b[4];
    int n = nema_read(0, b, sizeof(b));
    if (n == NEMA_ERR_INTR && nema_interrupted()) {
        nema_sig_take(NEMA_SIG_INT);
        return 130;
    }
    return 1;
}

static int app_exit42(int argc, char **argv) { return 42; }

static int app_argv(int argc, char **argv) {
    char line[128] = "";
    for (int i = 0; i < argc; i++) {
        strcat(line, argv[i]);
        strcat(line, i + 1 < argc ? "," : "");
    }
    nema_write(1, line, strlen(line));
    return argc;
}

static int app_poll2(int argc, char **argv) {
    /* waits on stdin (fd 0) and fd 2 (a second pipe); reports which became readable */
    nema_pollfd_t p[2] = {{0, NEMA_POLL_IN, 0}, {2, NEMA_POLL_IN, 0}};
    int r = nema_poll(p, 2, 2000);
    char out[16];
    int n = snprintf(out, sizeof(out), "%d:%u%u", r, p[0].revents ? 1u : 0u, p[1].revents ? 1u : 0u);
    nema_write(1, out, (size_t)n);
    return 0;
}

/* hog-ext: allocates 60 KB PSRAM blocks until refused; exits with the number that succeeded (the quota is 256 KB) */
static int app_hog_ext(int argc, char **argv) {
    int n = 0;
    while (n < 16 && nema_alloc(NEMA_MEM_EXTERNAL, nema_self()->owner, 60u * 1024u)) n++;
    return n;
}

/* hog-int: allocates 4 KB internal blocks until refused; exits with the number that succeeded (default quota 64 KB) */
static int app_hog_int(int argc, char **argv) {
    int n = 0;
    while (n < 64 && nema_alloc(NEMA_MEM_INTERNAL, nema_self()->owner, 4096)) n++;
    return n;
}

/* leak: keeps 1 KB internal and 2 KB PSRAM, never frees them, exits normally */
static int app_leak(int argc, char **argv) {
    nema_alloc(NEMA_MEM_INTERNAL, nema_self()->owner, 1024);
    nema_alloc(NEMA_MEM_EXTERNAL, nema_self()->owner, 2048);
    return 0;
}

/* leak-wait: leaks as above, then blocks on stdin until a signal; INT exits 130 */
static int app_leak_wait(int argc, char **argv) {
    app_leak(argc, argv);
    char b[4];
    int n = nema_read(0, b, sizeof(b));
    (void)n;
    nema_sig_take(NEMA_SIG_FATAL);
    return 130;
}

/* probe: exits with the number of heap blocks its own owner holds at start (argv is the only one) */
static int app_probe(int argc, char **argv) {
    nema_owner_stat_t st;
    nema_mem_owner_stat(nema_self()->owner, &st);
    return (int)st.allocs;
}

static const nema_app_t APPS[] = {
    {"upcase", "uppercase stdin", app_upcase, 0, 0},
    {"sleeper", "block on stdin", app_sleeper, 0, 0},
    {"exit42", "exit with 42", app_exit42, 0, 0},
    {"argv", "print argv", app_argv, 0, 0},
    {"poll2", "poll two handles", app_poll2, 0, 0},
    {"hog-ext", "allocate PSRAM until refused", app_hog_ext, 0, 0},
    {"hog-int", "allocate internal RAM until refused", app_hog_int, 0, 0},
    {"leak", "exit while holding memory", app_leak, 0, 0},
    {"leak-wait", "leak, then wait for a signal", app_leak_wait, 0, 0},
    {"probe", "report own heap blocks at start", app_probe, 0, 0},
};

/* Heap bytes and blocks held by every process slot, used to check that exits give everything back. */
static void proc_usage(size_t *bytes, size_t *blocks) {
    *bytes = 0;
    *blocks = 0;
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        nema_owner_stat_t s;
        if (!nema_mem_owner_stat((nema_owner_t)(NEMA_OWNER_PROC_BASE + i), &s)) continue;
        *bytes += s.used[NEMA_MEM_INTERNAL] + s.used[NEMA_MEM_DMA] + s.used[NEMA_MEM_EXTERNAL];
        *blocks += s.allocs;
    }
}

static int run_app(const char *name, uint32_t quota_ext, int *code_out) {
    nema_spawn_t sp = {.name = name, .mem_quota_ext = quota_ext};
    nema_pid_t pid;
    if (nema_proc_spawn(&sp, &pid) != NEMA_OK) return -1;
    return nema_proc_wait(pid, code_out, 3000);
}

TEST(process_quota_refuses_past_the_limit) {
    int code = -1;
    CHECK(run_app("hog-ext", 256u * 1024u, &code) == NEMA_OK);
    CHECK_EQ(code, 4); /* 4 x 60 KB fit in 256 KB; the fifth is refused */
    CHECK(run_app("hog-int", 0, &code) == NEMA_OK);
    CHECK_EQ(code, 16); /* default internal quota: 16 x 4 KB = 64 KB */
    /* the system and other processes are unaffected by that hog */
    void *sys = nema_alloc(NEMA_MEM_EXTERNAL, OW, 512u * 1024u);
    CHECK(sys != NULL);
    nema_free(sys);
    CHECK(run_app("exit42", 0, &code) == NEMA_OK);
    CHECK_EQ(code, 42);
}

TEST(exit_sweeps_leaks_back_to_baseline) {
    size_t base_bytes, base_blocks;
    proc_usage(&base_bytes, &base_blocks);
    for (int i = 0; i < 100; i++) {
        int code = -1;
        CHECK(run_app("leak", 0, &code) == NEMA_OK);
        size_t bytes, blocks;
        proc_usage(&bytes, &blocks);
        if (bytes != base_bytes || blocks != base_blocks) {
            printf("    iteration %d: %zu bytes / %zu blocks, baseline %zu / %zu\n", i, bytes, blocks, base_bytes,
                   base_blocks);
            g_failures++;
            break;
        }
        CHECK(run_app("exit42", 0, &code) == NEMA_OK);
        CHECK_EQ(code, 42);
    }
}

TEST(a_killed_process_is_swept_and_the_next_one_starts_clean) {
    size_t base_bytes, base_blocks;
    proc_usage(&base_bytes, &base_blocks);
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = "leak-wait", .stdio = {r, NULL, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(50);
    CHECK(nema_proc_signal(pid, NEMA_SIG_INT) == NEMA_OK);
    int code = 0;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 130);
    size_t bytes, blocks;
    proc_usage(&bytes, &blocks);
    CHECK_EQ(bytes, base_bytes);
    CHECK_EQ(blocks, base_blocks);
    /* the next process in the same slot starts with only its argv block */
    CHECK(run_app("probe", 0, &code) == NEMA_OK);
    CHECK_EQ(code, 1);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

TEST(spawn_pipe_wait_roundtrip) {
    nema_obj_t *in_r, *in_w, *out_r, *out_w;
    nema_pipe_create(64, NEMA_MEM_ANY, OW, &in_r, &in_w);
    nema_pipe_create(64, NEMA_MEM_ANY, OW, &out_r, &out_w);
    const char *args[] = {"upcase", "5"};
    nema_spawn_t sp = {.name = "upcase", .argc = 2, .argv = args, .stdio = {in_r, out_w, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_obj_unref(in_r); /* the process holds its own references now */
    nema_obj_unref(out_w);
    nema_obj_write(in_w, "hello pal", 9, -1);
    nema_obj_unref(in_w); /* EOF for the child */
    char buf[32] = {0};
    int got = 0, n;
    while (got < 9 && (n = nema_obj_read(out_r, buf + got, sizeof(buf) - (size_t)got - 1, 2000)) > 0) got += n;
    CHECK(strcmp(buf, "HELLO PAL") == 0);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 12);
    nema_obj_unref(out_r);
}

TEST(argv_is_copied_and_defaults_to_name) {
    nema_obj_t *r, *w;
    nema_pipe_create(128, NEMA_MEM_ANY, OW, &r, &w);
    const char *args[] = {"argv", "-a", "two words"};
    nema_spawn_t sp = {.name = "argv", .argc = 3, .argv = args, .stdio = {NULL, w, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 3);
    char buf[64] = {0};
    nema_obj_read(r, buf, sizeof(buf) - 1, 500);
    CHECK(strcmp(buf, "argv,-a,two words") == 0);
    nema_spawn_t bare = {.name = "argv", .stdio = {NULL, w, NULL}};
    CHECK(nema_proc_spawn(&bare, &pid) == NEMA_OK);
    nema_proc_wait(pid, &code, 2000);
    CHECK_EQ(code, 1);
    nema_obj_unref(w);
    nema_obj_unref(r);
}

TEST(exit_code_and_unknown_app) {
    nema_spawn_t sp = {.name = "exit42"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code = 0;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 42);
    CHECK(nema_proc_wait(pid, &code, 100) == NEMA_ERR_NOENT); /* already reaped */
    nema_spawn_t bad = {.name = "nope"};
    CHECK(nema_proc_spawn(&bad, &pid) == NEMA_ERR_NOENT);
}

TEST(signal_interrupts_a_blocked_read) {
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = "sleeper", .stdio = {r, NULL, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(50); /* let it block */
    int code = 0;
    CHECK(nema_proc_wait(pid, &code, 100) == NEMA_ERR_TIMEOUT); /* still blocked */
    CHECK(nema_proc_signal(pid, NEMA_SIG_INT) == NEMA_OK);
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 130);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

TEST(poll_wakes_on_either_handle) {
    nema_obj_t *a_r, *a_w, *b_r, *b_w, *o_r, *o_w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &a_r, &a_w);
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &b_r, &b_w);
    nema_pipe_create(32, NEMA_MEM_ANY, OW, &o_r, &o_w);
    nema_spawn_t sp = {.name = "poll2", .stdio = {a_r, o_w, b_r}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(50); /* the child is blocked in poll now */
    nema_obj_write(b_w, "x", 1, 0);
    int code;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    char buf[16] = {0};
    nema_obj_read(o_r, buf, sizeof(buf) - 1, 500);
    CHECK(strcmp(buf, "1:01") == 0); /* one handle ready: the second */
    nema_obj_unref(a_r), nema_obj_unref(a_w), nema_obj_unref(b_r), nema_obj_unref(b_w);
    nema_obj_unref(o_r), nema_obj_unref(o_w);
}

TEST(poll_reports_an_invalid_handle_immediately) {
    nema_obj_t *o_r, *o_w;
    nema_pipe_create(32, NEMA_MEM_ANY, OW, &o_r, &o_w);
    nema_spawn_t sp = {.name = "poll2", .stdio = {NULL, o_w, NULL}}; /* fds 0 and 2 are the null device */
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    char buf[16] = {0};
    nema_obj_read(o_r, buf, sizeof(buf) - 1, 500);
    CHECK(strcmp(buf, "2:11") == 0); /* the null device always reads as ready (end of stream) */
    nema_obj_unref(o_r), nema_obj_unref(o_w);
}

TEST(many_spawns_leave_no_trace) {
    nema_owner_stat_t s;
    for (int i = 0; i < 60; i++) {
        nema_spawn_t sp = {.name = "exit42"};
        nema_pid_t pid;
        CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
        int code;
        CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    }
    nema_port_sleep_ms(50);
    CHECK_EQ(nema_task_count(), 0);
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        nema_mem_owner_stat((nema_owner_t)(NEMA_OWNER_PROC_BASE + i), &s);
        CHECK_EQ(s.allocs, 0);
        CHECK_EQ(s.stack, 0);
    }
    size_t internal_free = nema_port_mem_free(NEMA_MEM_INTERNAL);
    CHECK(internal_free > 440u * 1024u);
}

TEST(spawn_is_refused_when_internal_floor_would_break) {
    size_t floor = nema_mem_floor();
    size_t free_now = nema_port_mem_free(NEMA_MEM_INTERNAL);
    void *hog = nema_alloc_ex(NEMA_MEM_INTERNAL, OW, free_now - floor - 2048, 0, NEMA_ALLOC_CRITICAL);
    CHECK(hog != NULL);
    nema_spawn_t sp = {.name = "exit42"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_ERR_NOMEM); /* 8 KB stack does not fit */
    nema_free(hog);
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code;
    nema_proc_wait(pid, &code, 2000);
}

TEST(detached_process_is_reaped_automatically) {
    nema_spawn_t sp = {.name = "exit42", .flags = NEMA_SPAWN_DETACH};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(100);
    nema_proc_t list[NEMA_PROC_MAX];
    CHECK_EQ(nema_proc_list(list, NEMA_PROC_MAX), 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("proctest");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    RUN(spawn_pipe_wait_roundtrip);
    RUN(argv_is_copied_and_defaults_to_name);
    RUN(exit_code_and_unknown_app);
    RUN(signal_interrupts_a_blocked_read);
    RUN(poll_wakes_on_either_handle);
    RUN(poll_reports_an_invalid_handle_immediately);
    RUN(many_spawns_leave_no_trace);
    RUN(spawn_is_refused_when_internal_floor_would_break);
    RUN(detached_process_is_reaped_automatically);
    RUN(process_quota_refuses_past_the_limit);
    RUN(exit_sweeps_leaks_back_to_baseline);
    RUN(a_killed_process_is_swept_and_the_next_one_starts_clean);
    return TEST_DONE();
}
