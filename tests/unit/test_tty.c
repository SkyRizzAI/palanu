#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "test.h"

static nema_owner_t OW;

/* readline: reads one line from stdin and answers "got:<line>"; returns its length, -1 on EOF */
static int app_readline(int argc, char **argv) {
    char b[64];
    int n = nema_read(0, b, sizeof(b) - 1);
    if (n <= 0) {
        nema_write(1, "eof\n", 4);
        return -1;
    }
    b[n] = 0;
    char out[96];
    int m = snprintf(out, sizeof(out), "got:%s", b);
    nema_write(1, out, (size_t)m);
    return n;
}

/* spin: runs until interrupted, then reports through its exit code */
static int app_spin(int argc, char **argv) {
    while (!nema_interrupted()) nema_port_sleep_ms(5);
    nema_sig_take(NEMA_SIG_INT);
    return 99;
}

/* rawecho: puts the tty in raw mode and echoes N bytes back unchanged */
static int app_rawecho(int argc, char **argv) {
    uint32_t mode = NEMA_TTY_RAW;
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    nema_write(1, "R", 1);
    char b[8];
    int total = 0;
    while (total < 4) {
        int n = nema_read(0, b, sizeof(b));
        if (n <= 0) break;
        nema_write(1, b, (size_t)n);
        total += n;
    }
    return total;
}

static int app_winsize(int argc, char **argv) {
    nema_winsize_t ws;
    char out[32];
    for (int i = 0; i < 2; i++) {
        nema_ioctl(0, NEMA_TIOC_GETSIZE, &ws);
        int n = snprintf(out, sizeof(out), "%ux%u;", ws.cols, ws.rows);
        nema_write(1, out, (size_t)n);
        nema_pollfd_t p = {0, NEMA_POLL_IN, 0};
        int r = nema_poll(&p, 1, 2000); /* returns INTR when the window changes */
        if (r == NEMA_ERR_INTR) nema_sig_take(NEMA_SIG_WINCH);
    }
    return 0;
}

static const nema_app_t APPS[] = {
    {"readline", "", app_readline, 0, 0}, {"spin", "", app_spin, 0, 0}, {"rawecho", "", app_rawecho, 0, 0},
    {"winsize", "", app_winsize, 0, 0},
};

static int collect(nema_tty_t *t, char *buf, size_t cap, int32_t quiet_ms) {
    size_t n = 0;
    for (;;) {
        int r = nema_tty_output_read(t, buf + n, cap - n - 1, quiet_ms);
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = 0;
    return (int)n;
}

TEST(cooked_line_editing_and_echo) {
    nema_tty_t *t;
    CHECK(nema_tty_create(&t, 40, 15, OW) == NEMA_OK);
    nema_spawn_t sp = {.name = "readline", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    nema_tty_set_foreground(t, pid);
    nema_tty_input(t, "helx\x7f" "lo", 7); /* hel, x, DEL, lo */
    nema_tty_input(t, "\r", 1);
    int code = 0;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 6); /* "hello\n" */
    char out[128];
    collect(t, out, sizeof(out), 100);
    CHECK(strstr(out, "helx\b \blo\r\n") != NULL); /* echo including the erase sequence and CRLF */
    CHECK(strstr(out, "got:hello\r\n") != NULL || strstr(out, "got:hello\n") != NULL);
    nema_tty_destroy(t);
}

TEST(ctrl_c_interrupts_foreground) {
    nema_tty_t *t;
    nema_tty_create(&t, 40, 15, OW);
    nema_spawn_t sp = {.name = "spin", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    nema_proc_spawn(&sp, &pid);
    nema_tty_set_foreground(t, pid);
    nema_port_sleep_ms(30);
    nema_tty_input(t, "\x03", 1);
    int code = 0;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 99);
    char out[32];
    collect(t, out, sizeof(out), 50);
    CHECK(strstr(out, "^C") != NULL);
    nema_tty_destroy(t);
}

TEST(raw_mode_passes_every_byte) {
    nema_tty_t *t;
    nema_tty_create(&t, 40, 15, OW);
    nema_spawn_t sp = {.name = "rawecho", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    nema_proc_spawn(&sp, &pid);
    char out[64];
    char first;
    CHECK_EQ(nema_tty_output_read(t, &first, 1, 2000), 1); /* wait for the "R" so raw mode is active */
    CHECK_EQ(first, 'R');
    nema_tty_input(t, "\x03\x04\x1b\n", 4); /* Ctrl-C, Ctrl-D, Esc, LF: all plain bytes in raw mode */
    int code = 0;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 4);
    int n = collect(t, out, sizeof(out), 50);
    CHECK_EQ(n, 4);
    CHECK(memcmp(out, "\x03\x04\x1b\n", 4) == 0); /* no echo of ours, no CRLF translation */
    nema_tty_destroy(t);
}

TEST(ctrl_d_on_empty_line_is_eof) {
    nema_tty_t *t;
    nema_tty_create(&t, 40, 15, OW);
    nema_spawn_t sp = {.name = "readline", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    nema_proc_spawn(&sp, &pid);
    nema_port_sleep_ms(30);
    nema_tty_input(t, "\x04", 1);
    int code = 0;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, -1);
    nema_tty_destroy(t);
}

TEST(window_size_and_resize_signal) {
    nema_tty_t *t;
    nema_tty_create(&t, 64, 30, OW);
    nema_spawn_t sp = {.name = "winsize", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    nema_proc_spawn(&sp, &pid);
    nema_tty_set_foreground(t, pid);
    char first[16] = {0};
    CHECK(nema_tty_output_read(t, first, sizeof(first) - 1, 2000) > 0);
    CHECK(strcmp(first, "64x30;") == 0);
    nema_tty_set_size(t, 53, 20);
    int code;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK);
    char out[32];
    collect(t, out, sizeof(out), 50);
    CHECK(strcmp(out, "53x20;") == 0);
    nema_tty_destroy(t);
}

TEST(tty_releases_all_memory) {
    nema_port_sleep_ms(50);
    nema_owner_stat_t s;
    nema_mem_owner_stat(OW, &s);
    CHECK_EQ(s.allocs, 0);
    CHECK_EQ(nema_task_count(), 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("ttytest");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    RUN(cooked_line_editing_and_echo);
    RUN(ctrl_c_interrupts_foreground);
    RUN(raw_mode_passes_every_byte);
    RUN(ctrl_d_on_empty_line_is_eof);
    RUN(window_size_and_resize_signal);
    RUN(tty_releases_all_memory);
    return TEST_DONE();
}
