/* The events module (pal_wait, the process event queue) and the terminal modes of the palanu API, end to end: the
 * hand-encoded apps of tests/unit/wasm_fixtures_core.h (made by tests/unit/tools/gen_core_fixtures.py) call the imports,
 * the glue checks their capabilities, and the test drives the terminal and the pipes from outside the apps. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/papp.h"
#include "test.h"
#include "wasm_fixtures_core.h"

static nema_owner_t OW;

/* Runs mod as the calling process, with the given manifest permissions (the image is copied into the process, as the
 * loader expects). */
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
#define APP_FN(fn, mod, id) \
    static int fn(int argc, char **argv) { return run_mod(mod, sizeof(mod), id, PERM_PROC, argc, argv); }

APP_FN(a_wait_timeout, WASM_WAIT_TIMEOUT, "api-wait-timeout")
APP_FN(a_wait_stdin, WASM_WAIT_STDIN, "api-wait-stdin")
APP_FN(a_wait_bad, WASM_WAIT_BAD, "api-wait-bad")
APP_FN(a_wait_only, WASM_WAIT_ONLY, "api-wait-only")
APP_FN(a_resize_keep, WASM_RESIZE_KEEP, "api-resize-keep")
APP_FN(a_events_wait, WASM_EVENTS_WAIT, "api-events-wait")
APP_FN(a_tty_raw, WASM_TTY_RAW, "api-tty-raw")
APP_FN(a_tty_nosig, WASM_TTY_NOSIG, "api-tty-nosig")
APP_FN(a_tty_noecho, WASM_TTY_NOECHO, "api-tty-noecho")
APP_FN(a_tty_cooked, WASM_TTY_COOKED, "api-tty-cooked")
APP_FN(a_tty_get, WASM_TTY_GET, "api-tty-get")
APP_FN(a_term_size, WASM_TERM_SIZE, "api-term-size")

static const nema_app_t APPS[] = {
    {"api-wait-timeout", "", a_wait_timeout, 0, 0},   {"api-wait-stdin", "", a_wait_stdin, 0, 0},
    {"api-wait-bad", "", a_wait_bad, 0, 0},           {"api-wait-only", "", a_wait_only, 0, 0},
    {"api-resize-keep", "", a_resize_keep, 0, 0},     {"api-events-wait", "", a_events_wait, 0, 0},
    {"api-tty-raw", "", a_tty_raw, 0, 0},             {"api-tty-nosig", "", a_tty_nosig, 0, 0},
    {"api-tty-noecho", "", a_tty_noecho, 0, 0},       {"api-tty-cooked", "", a_tty_cooked, 0, 0},
    {"api-tty-get", "", a_tty_get, 0, 0},             {"api-term-size", "", a_term_size, 0, 0},
};

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }

/* A pipe with a read end and a write end, big enough that an app never blocks on it in these tests. */
static void make_pipe(nema_obj_t **rd, nema_obj_t **wr) { nema_pipe_create(4096, NEMA_MEM_ANY, OW, rd, wr); }

/* Starts a registered app with the given stdin and stdout (NULL inherits the test's, which is the null device). */
static nema_pid_t start(const char *name, nema_obj_t *in, nema_obj_t *out) {
    nema_spawn_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.name = name;
    sp.stdio[0] = in;
    sp.stdio[1] = out;
    nema_pid_t pid = 0;
    return nema_proc_spawn(&sp, &pid) == NEMA_OK ? pid : 0;
}

/* Reads until the writers are gone (end of input) or nothing comes for a second. */
static size_t drain(nema_obj_t *rd, uint8_t *buf, size_t cap) {
    size_t n = 0;
    while (n < cap) {
        int k = nema_obj_read(rd, buf + n, cap - n, 1000);
        if (k <= 0) break;
        n += (size_t)k;
    }
    return n;
}

/* Waits for the terminal's mode to become want (the app sets it on its own task): up to a second. */
static bool wait_mode(nema_tty_t *t, uint32_t want) {
    nema_obj_t *o = nema_tty_proc_obj(t);
    for (int i = 0; i < 200; i++) {
        uint32_t m = ~want;
        o->ops->ioctl(o, NEMA_TIOC_GETMODE, &m);
        if (m == want) return true;
        nema_port_sleep_ms(5);
    }
    return false;
}

static uint32_t tty_flags(nema_tty_t *t) {
    nema_obj_t *o = nema_tty_proc_obj(t);
    uint32_t m = 0;
    o->ops->ioctl(o, NEMA_TIOC_GETMODE, &m);
    return m;
}

/* ---- pal_wait ---- */

TEST(wait_returns_zero_when_nothing_is_ready_before_the_timeout) {
    nema_obj_t *ir, *iw, *orr, *ow;
    make_pipe(&ir, &iw);
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-wait-timeout", ir, ow);
    nema_obj_unref(ow);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[32] = {0};
    size_t n = drain(orr, out, sizeof(out));
    CHECK_EQ(n, 20);
    CHECK_EQ((int32_t)le32(out), 0);         /* pal_wait returned 0 */
    CHECK_EQ(le16(out + 4 + 6), 0);          /* stdin: nothing ready */
    CHECK_EQ(le16(out + 4 + 8 + 6), 0);      /* the queue: no event */
    nema_obj_unref(iw);
    nema_obj_unref(ir);
    nema_obj_unref(orr);
}

TEST(wait_reports_the_ready_handle_when_data_arrives) {
    nema_obj_t *ir, *iw, *orr, *ow;
    make_pipe(&ir, &iw);
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-wait-stdin", ir, ow);
    nema_obj_unref(ow);
    nema_port_sleep_ms(60);
    CHECK(nema_obj_write(iw, "x", 1, 0) == 1);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[32] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 20);
    CHECK_EQ((int32_t)le32(out), 1);         /* one record ready */
    CHECK_EQ(le16(out + 4 + 6), 1);          /* stdin: PAL_POLL_IN */
    CHECK_EQ(le16(out + 4 + 8 + 6), 0);      /* the queue is not ready */
    nema_obj_unref(iw);
    nema_obj_unref(ir);
    nema_obj_unref(orr);
}

TEST(wait_refuses_records_that_are_not_whole_or_too_many) {
    nema_obj_t *ir, *iw, *orr, *ow;
    make_pipe(&ir, &iw);
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-wait-bad", ir, ow);
    nema_obj_unref(ow);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[8] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 8);
    CHECK_EQ((int32_t)le32(out), -2);        /* 12 bytes: not a multiple of 8 */
    CHECK_EQ((int32_t)le32(out + 4), -2);    /* 17 records: more than 16 */
    nema_obj_unref(iw);
    nema_obj_unref(ir);
    nema_obj_unref(orr);
}

TEST(a_stop_request_ends_a_wait_that_does_not_list_the_queue) {
    /* a read-like wait on the terminal: Ctrl-C ends the app with the interrupted status (130), as it does for read */
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_pid_t pid = start("api-wait-only", nema_tty_proc_obj(t), NULL);
    nema_tty_set_foreground(t, pid);
    nema_port_sleep_ms(60);
    nema_tty_input(t, "\x03", 1);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 130);
    nema_tty_destroy(t);
}

TEST(a_window_change_nobody_listened_for_is_kept_for_the_queue) {
    /* the app waits 400 ms on the terminal without the queue; the resize at 80 ms is kept, and read from the queue after */
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-resize-keep", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    nema_tty_set_foreground(t, pid);
    nema_port_sleep_ms(80);
    nema_tty_set_size(t, 100, 40);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[24] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 24);
    CHECK_EQ((int32_t)le32(out), 0);         /* the wait timed out */
    CHECK_EQ((int32_t)le32(out + 4), 16);    /* the queue held the event */
    CHECK_EQ(le32(out + 8), 2u);             /* RESIZE */
    CHECK_EQ(le32(out + 12), 100u);          /* columns */
    CHECK_EQ(le32(out + 16), 40u);           /* rows */
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

TEST(a_window_change_reaches_the_app_through_the_queue) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-events-wait", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    nema_tty_set_foreground(t, pid);
    nema_port_sleep_ms(80);
    nema_tty_set_size(t, 100, 40);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[24] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 24);
    CHECK_EQ((int32_t)le32(out), 1);         /* the queue was ready */
    CHECK_EQ((int32_t)le32(out + 4), 16);    /* one event read */
    CHECK_EQ(le32(out + 8), 2u);
    CHECK_EQ(le32(out + 12), 100u);
    CHECK_EQ(le32(out + 16), 40u);
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

TEST(ctrl_c_reaches_the_app_through_the_queue_as_quit) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-events-wait", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    nema_tty_set_foreground(t, pid);
    nema_port_sleep_ms(80);
    nema_tty_input(t, "\x03", 1); /* cooked: Ctrl-C raises INT for the foreground group */
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);            /* the app read QUIT and returned, it was not stopped */
    uint8_t out[24] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 24);
    CHECK_EQ((int32_t)le32(out), 1);
    CHECK_EQ(le32(out + 8), 5u);             /* QUIT */
    CHECK_EQ(le32(out + 12), 1u);            /* a = INT */
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

/* ---- terminal modes ---- */

TEST(raw_mode_delivers_ctrl_c_as_a_byte_and_does_not_stop_the_app) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-tty-raw", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    nema_tty_set_foreground(t, pid);
    CHECK(wait_mode(t, NEMA_TTY_RAW));
    nema_tty_input(t, "\x03", 1);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);                       /* not 130: Ctrl-C did not interrupt the app */
    uint8_t out[24] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 24);
    CHECK_EQ((int32_t)le32(out), 0);         /* pal_tty_mode */
    CHECK_EQ((int32_t)le32(out + 4), 1);     /* one byte read */
    CHECK_EQ(out[8], 0x03);
    CHECK_EQ(tty_flags(t), (uint32_t)NEMA_TTY_COOKED); /* put back when the app ended */
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

TEST(nosig_mode_is_raw_here_so_ctrl_c_is_a_byte) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-tty-nosig", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    nema_tty_set_foreground(t, pid);
    CHECK(wait_mode(t, NEMA_TTY_RAW));
    nema_tty_input(t, "\x03", 1);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[24] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 24);
    CHECK_EQ(out[8], 0x03);
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

TEST(noecho_reads_a_line_without_echoing_it) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-tty-noecho", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    nema_tty_set_foreground(t, pid);
    CHECK(wait_mode(t, NEMA_TTY_COOKED & ~NEMA_TTY_ECHO));
    nema_tty_input(t, "pw\n", 3);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[24] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 24);
    CHECK_EQ((int32_t)le32(out + 4), 3);     /* "pw\n" read as one line */
    CHECK(memcmp(out + 8, "pw\n", 3) == 0);
    uint8_t echo[64];
    CHECK_EQ(nema_tty_output_read(t, echo, sizeof(echo), 0) <= 0, 1); /* nothing was echoed to the terminal */
    CHECK_EQ(tty_flags(t), (uint32_t)NEMA_TTY_COOKED);
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

TEST(cooked_mode_echoes_the_same_line_so_noecho_is_not_a_no_op) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-tty-cooked", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    nema_tty_set_foreground(t, pid);
    CHECK(wait_mode(t, NEMA_TTY_COOKED));
    nema_tty_input(t, "pw\n", 3);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    uint8_t echo[64] = {0};
    int n = nema_tty_output_read(t, echo, sizeof(echo) - 1, 0);
    CHECK(n > 0);
    CHECK(strstr((char *)echo, "pw\r\n") != NULL);
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

TEST(tty_get_reports_the_mode_and_caps_are_all_set) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 80, 24, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-tty-get", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    CHECK_EQ(code, 0);
    uint8_t out[28] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 28);
    CHECK_EQ((int32_t)le32(out), 0);         /* cooked before the app changed it */
    CHECK_EQ((int32_t)le32(out + 8), 4);     /* NOECHO after tty_raw(4) */
    CHECK_EQ((int32_t)le32(out + 16), 1);    /* RAW after tty_raw(1) */
    CHECK_EQ((int32_t)le32(out + 24), 7);    /* 256 colours, UTF-8, alternate screen */
    CHECK_EQ(tty_flags(t), (uint32_t)NEMA_TTY_COOKED);
    nema_obj_unref(orr);
    nema_tty_destroy(t);
}

TEST(term_size_is_the_controlling_terminal_stdin_or_stdout) {
    /* stdin is the terminal: its size is read from stdin */
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 132, 43, OW) == NEMA_OK);
    nema_obj_t *orr, *ow;
    make_pipe(&orr, &ow);
    nema_pid_t pid = start("api-term-size", nema_tty_proc_obj(t), ow);
    nema_obj_unref(ow);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    uint8_t out[8] = {0};
    CHECK_EQ(drain(orr, out, sizeof(out)), 8);
    CHECK_EQ(le16(out + 4), 132u);
    CHECK_EQ(le16(out + 6), 43u);
    nema_obj_unref(orr);
    nema_tty_destroy(t);

    /* stdin is a pipe and stdout the terminal: the size comes from stdout (the next fd that is a terminal) */
    CHECK(nema_tty_create(&t, 132, 43, OW) == NEMA_OK);
    nema_obj_t *ir, *iw;
    make_pipe(&ir, &iw);
    pid = start("api-term-size", ir, nema_tty_proc_obj(t));
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    uint8_t echo[16] = {0};
    int n = nema_tty_output_read(t, echo, sizeof(echo), 500);
    CHECK_EQ(n, 8);
    CHECK_EQ(le16(echo + 4), 132u);
    CHECK_EQ(le16(echo + 6), 43u);
    nema_obj_unref(iw);
    nema_obj_unref(ir);
    nema_tty_destroy(t);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("apievents");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    RUN(wait_returns_zero_when_nothing_is_ready_before_the_timeout);
    RUN(wait_reports_the_ready_handle_when_data_arrives);
    RUN(wait_refuses_records_that_are_not_whole_or_too_many);
    RUN(a_stop_request_ends_a_wait_that_does_not_list_the_queue);
    RUN(a_window_change_nobody_listened_for_is_kept_for_the_queue);
    RUN(a_window_change_reaches_the_app_through_the_queue);
    RUN(ctrl_c_reaches_the_app_through_the_queue_as_quit);
    RUN(raw_mode_delivers_ctrl_c_as_a_byte_and_does_not_stop_the_app);
    RUN(nosig_mode_is_raw_here_so_ctrl_c_is_a_byte);
    RUN(noecho_reads_a_line_without_echoing_it);
    RUN(cooked_mode_echoes_the_same_line_so_noecho_is_not_a_no_op);
    RUN(tty_get_reports_the_mode_and_caps_are_all_set);
    RUN(term_size_is_the_controlling_terminal_stdin_or_stdout);
    return TEST_DONE();
}
