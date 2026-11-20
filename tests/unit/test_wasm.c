/* WebAssembly apps as processes: the module image, instance, exec stack and linear memory are charged to
 * the process; memory.grow is capped by the manifest; the file image is freed after load; a quota refusal is clean.
 * The modules are hand-encoded here (tests/unit/wasm_fixtures.h) so the test needs no toolchain. */
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "test.h"
#include "wasm_fixtures.h"

static nema_owner_t OW;
static const uint8_t *g_big;   /* a READ module with a 200 KB custom section, set by the test before spawn */
static size_t g_big_len;

/* Runs `mod` as the calling process: copies it into a process-charged image, then hands the image to the runtime. */
static int host(const uint8_t *mod, size_t n, uint32_t mem_kb, uint32_t fuel_m, int argc, char **argv) {
    uint8_t *img = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, n);
    if (!img) return 125;
    memcpy(img, mod, n);
    pal_papp_t app;
    memset(&app, 0, sizeof(app));
    strcpy(app.id, "test");
    app.api = 1;
    app.mem_kb = mem_kb;
    app.fuel_m = fuel_m;
    app.stack_kb = 16;
    app.payload = img;
    app.payload_len = n;
    return pal_wasm_run(&app, img, argc, argv);
}

static int app_grow_128(int argc, char **argv) { return host(WASM_GROW, sizeof(WASM_GROW), 128, 0, argc, argv); }
static int app_grow_256(int argc, char **argv) { return host(WASM_GROW, sizeof(WASM_GROW), 256, 0, argc, argv); }
static int app_read(int argc, char **argv) { return host(WASM_READ, sizeof(WASM_READ), 64, 0, argc, argv); }
static int app_read_big(int argc, char **argv) { return host(g_big, g_big_len, 64, 0, argc, argv); }
static int app_pure(int argc, char **argv) { return host(WASM_PURE_LOOP, sizeof(WASM_PURE_LOOP), 64, 5, argc, argv); }
static int app_pure_nofuel(int argc, char **argv) { return host(WASM_PURE_LOOP, sizeof(WASM_PURE_LOOP), 64, 0, argc, argv); }
static int app_sleep(int argc, char **argv) { return host(WASM_SLEEP_LOOP, sizeof(WASM_SLEEP_LOOP), 64, 0, argc, argv); }
static int app_oob(int argc, char **argv) { return host(WASM_OOB_STORE, sizeof(WASM_OOB_STORE), 64, 0, argc, argv); }

static const nema_app_t APPS[] = {
    {"wasm-grow-128", "grow past a 128 KB manifest", app_grow_128, 0, 0},
    {"wasm-grow-256", "grow within a 256 KB manifest", app_grow_256, 0, 0},
    {"wasm-read", "block on stdin, then exit", app_read, 0, 0},
    {"wasm-read-big", "block on stdin, large image", app_read_big, 0, 0},
    {"wasm-pure", "pure loop, stopped by fuel", app_pure, 0, 0},
    {"wasm-pure-nofuel", "pure loop, no fuel: only a kill stops it", app_pure_nofuel, 0, 0},
    {"wasm-sleep", "loop calling sleep_ms", app_sleep, 0, 0},
    {"wasm-oob", "out-of-bounds store", app_oob, 0, 0},
};

/* Bytes held by every process slot (internal, DMA, and PSRAM), to check that usage comes and goes. */
static size_t proc_bytes(void) {
    size_t total = 0;
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        nema_owner_stat_t s;
        if (!nema_mem_owner_stat((nema_owner_t)(NEMA_OWNER_PROC_BASE + i), &s)) continue;
        total += s.used[NEMA_MEM_INTERNAL] + s.used[NEMA_MEM_DMA] + s.used[NEMA_MEM_EXTERNAL] + s.stack;
    }
    return total;
}

/* proc_bytes() once it has come back to want, or after a second: a process's stack is released when its task ends, a moment
 * after nema_proc_wait has returned the exit code, so a slow machine still sees it right after the wait. */
static size_t settled_bytes(size_t want) {
    size_t b = proc_bytes();
    for (int i = 0; i < 100 && b != want; i++) {
        nema_port_sleep_ms(10);
        b = proc_bytes();
    }
    return b;
}

static nema_owner_t owner_of(nema_pid_t pid) {
    nema_proc_t *p = nema_proc_get(pid);
    return p ? p->owner : NEMA_OWNER_KERNEL;
}

static size_t bytes_of(nema_owner_t o) {
    nema_owner_stat_t s;
    if (!nema_mem_owner_stat(o, &s)) return 0;
    return s.used[NEMA_MEM_INTERNAL] + s.used[NEMA_MEM_DMA] + s.used[NEMA_MEM_EXTERNAL];
}

TEST(grow_within_the_manifest_limit_succeeds) {
    nema_spawn_t sp = {.name = "wasm-grow-256"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_owner_t o = owner_of(pid);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    nema_owner_stat_t s;
    CHECK(nema_mem_owner_stat(o, &s));
    CHECK(s.peak >= 256u * 1024u); /* four 64 KB pages were charged to the process */
}

TEST(growth_beyond_the_manifest_limit_fails_cleanly) {
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_spawn_t sp = {.name = "wasm-grow-128"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_owner_t o = owner_of(pid);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 9); /* memory.grow returned -1; the module reports it and exits normally */
    nema_owner_stat_t s;
    CHECK(nema_mem_owner_stat(o, &s));
    CHECK(s.peak < 256u * 1024u); /* the grown pages were never allocated */
    CHECK_EQ(settled_bytes(base), base);
}

TEST(process_usage_returns_to_baseline_after_exit) {
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    for (int i = 0; i < 5; i++) {
        nema_spawn_t sp = {.name = "wasm-grow-256"};
        nema_pid_t pid;
        CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
        int code = -1;
        CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
        CHECK_EQ(code, 0);
        CHECK_EQ(settled_bytes(base), base);
    }
}

TEST(a_blocked_app_is_charged_while_it_runs) {
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = "wasm-read", .stdio = {r, NULL, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(80); /* it is inside read() now: image gone, instance and one linear page held */
    nema_owner_t o = owner_of(pid);
    CHECK(bytes_of(o) >= 64u * 1024u);
    nema_obj_unref(w); /* end of input: the read returns 0 and the module exits */
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    CHECK_EQ(settled_bytes(base), base);
    nema_obj_unref(r);
}

TEST(the_file_image_is_freed_after_load) {
    /* A module of about 200 KB (a custom section of zeros). If the image stayed alive during the run, the app would
     * hold about 200 KB more than its instance does (64 KB linear memory, 16 KB stack, a few KB of module state). */
    static uint8_t big[200u * 1024u + 128u];
    size_t n = sizeof(WASM_READ);
    memcpy(big, WASM_READ, n);
    big[n++] = 0x00;                       /* custom section */
    size_t body = 1 + 3 + 200u * 1024u;    /* name length, "pad", payload */
    uint32_t v = (uint32_t)body;           /* LEB128 of a value below 2^21 */
    while (v >= 0x80) { big[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    big[n++] = (uint8_t)v;
    big[n++] = 3;
    big[n++] = 'p', big[n++] = 'a', big[n++] = 'd';
    memset(big + n, 0, 200u * 1024u);
    n += 200u * 1024u;
    g_big = big;
    g_big_len = n;
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = "wasm-read-big", .stdio = {r, NULL, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(120);
    size_t held = bytes_of(owner_of(pid));
    CHECK(held >= 64u * 1024u);
    CHECK(held < 150u * 1024u); /* far below the image size */
    nema_obj_unref(w);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    nema_obj_unref(r);
}

TEST(a_quota_refusal_fails_the_app_cleanly) {
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    /* 16 KB of PSRAM cannot hold the 64 KB linear memory: the app stops with 126 and nothing stays charged */
    nema_spawn_t sp = {.name = "wasm-read", .stdio = {r, NULL, NULL}, .mem_quota_ext = 16u * 1024u};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 126);
    CHECK_EQ(settled_bytes(base), base);
    nema_obj_unref(w);
    nema_obj_unref(r);
}

TEST(fuel_stops_a_pure_loop_and_the_memory_comes_back) {
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_spawn_t sp = {.name = "wasm-pure"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 10000) == NEMA_OK);
    CHECK_EQ(code, 139); /* trapped: instruction limit exceeded */
    CHECK_EQ(settled_bytes(base), base);
}

static bool settle(size_t base) {
    for (int i = 0; i < 100; i++) {
        if (proc_bytes() == base) return true;
        nema_port_sleep_ms(5);
    }
    return proc_bytes() == base;
}

TEST(an_out_of_bounds_store_traps_inside_the_sandbox) {
    /* With the allocator hook there are no guard pages, so the bounds check is what keeps the store inside */
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_spawn_t sp = {.name = "wasm-oob"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 139);
    CHECK_EQ(settled_bytes(base), base);
}

TEST(kill_stops_a_wasm_loop_that_calls_imports) {
    /* the loop calls sleep_ms; the import sees the kill at once and the instance stops (docs/adr/0003) */
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_spawn_t sp = {.name = "wasm-sleep"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(60);
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_OK);
    CHECK_EQ(code, 137);
    CHECK(settle(base));
}

TEST(kill_stops_a_pure_wasm_loop_without_fuel) {
    /* no import call and no fuel: only the kill request that the interpreter checks at its metering points can stop it */
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_spawn_t sp = {.name = "wasm-pure-nofuel"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(80);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 0) != NEMA_OK); /* still spinning */
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 137);
    CHECK(settle(base));
}

TEST(kill_stops_a_wasm_app_blocked_in_read) {
    size_t base = settled_bytes(0); /* the processes of the test before have let go of their stacks */
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t sp = {.name = "wasm-read", .stdio = {r, NULL, NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_port_sleep_ms(60);
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 1000) == NEMA_OK);
    CHECK_EQ(code, 137);
    CHECK(settle(base));
    nema_obj_unref(w);
    nema_obj_unref(r);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("wasmtest");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    RUN(grow_within_the_manifest_limit_succeeds);
    RUN(growth_beyond_the_manifest_limit_fails_cleanly);
    RUN(process_usage_returns_to_baseline_after_exit);
    RUN(a_blocked_app_is_charged_while_it_runs);
    RUN(the_file_image_is_freed_after_load);
    RUN(a_quota_refusal_fails_the_app_cleanly);
    RUN(fuel_stops_a_pure_loop_and_the_memory_comes_back);
    RUN(an_out_of_bounds_store_traps_inside_the_sandbox);
    RUN(kill_stops_a_wasm_loop_that_calls_imports);
    RUN(kill_stops_a_pure_wasm_loop_without_fuel);
    RUN(kill_stops_a_wasm_app_blocked_in_read);
    return TEST_DONE();
}
