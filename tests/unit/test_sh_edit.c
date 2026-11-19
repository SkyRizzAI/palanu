/* The shell's line editor (apps/sh_edit.c). sh_readline reads fd 0 and writes fd 1, so the test runs it as a
 * real process on a TTY in raw mode: keys go in through nema_tty_input, and the echo and the returned lines
 * come back as output. Each line the editor returns is printed as [line], which is what the checks look for. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/history.h"
#include "sh.h"
#include "test.h"

static nema_owner_t OW;

static int app_edit(int argc, char **argv) {
    static sh_t SH;
    uint32_t mode = NEMA_TTY_RAW;
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    memset(&SH, 0, sizeof(SH));
    SH.owner = nema_self()->owner;
    SH.cols = 80;
    for (;;) {
        char line[SH_LINE_MAX];
        int n = sh_readline(&SH, "> ", line, sizeof(line));
        if (n < 0) {
            nema_write(1, "[eof]", 5);
            return 0;
        }
        char rec[SH_LINE_MAX + 4];
        int m = snprintf(rec, sizeof(rec), "[%s]", line);
        nema_write(1, rec, (size_t)m);
        sh_hist_add(&SH, line);
    }
}

/* An interactive shell whose command takes longer than the history delay: at the next prompt the write is overdue. */
static volatile int g_slow_reads;
static int app_slow(int argc, char **argv) {
    static sh_t SH;
    uint32_t mode = NEMA_TTY_RAW;
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    memset(&SH, 0, sizeof(SH));
    SH.owner = nema_self()->owner;
    SH.cols = 80;
    SH.interactive = true;
    char line[SH_LINE_MAX];
    if (sh_readline(&SH, "> ", line, sizeof(line)) < 0) return 1;
    sh_hist_add(&SH, line);
    nema_sleep_ms(PAL_HISTORY_IDLE_MS + 300); /* "the command runs" */
    int n = sh_readline(&SH, "> ", line, sizeof(line));
    return n < 0 ? 0 : 2;
}

static const nema_app_t APPS[] = {
    {"slowcmd", "", app_slow, 0, NEMA_APP_HIDDEN},
    {"editline", "", app_edit, 0, NEMA_APP_HIDDEN},
    {"echox", "", app_edit, 0, 0}, /* visible, so the first word can complete to it */
};

/* Read output until the first prompt: the editor is then in raw mode and ready for keys. */
static int until_prompt(nema_tty_t *t, char *buf, size_t cap) {
    size_t n = 0;
    while (!strstr(buf, "> ")) {
        int r = nema_tty_output_read(t, buf + n, cap - n - 1, 2000);
        if (r <= 0 || n + (size_t)r >= cap - 1) break;
        n += (size_t)r;
        buf[n] = 0;
    }
    return (int)n;
}

/* Run the editor with the given keys; the test must end its input with an empty-line Ctrl-D. Returns output. */
static int edit(const char *keys, size_t klen, char *out, size_t cap) {
    nema_tty_t *t;
    nema_tty_create(&t, 80, 24, OW);
    nema_spawn_t sp = {.name = "editline", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    nema_proc_spawn(&sp, &pid);
    nema_tty_set_foreground(t, pid);
    out[0] = 0;
    until_prompt(t, out, cap);
    nema_tty_input(t, keys, klen);
    int code = -1;
    nema_proc_wait(pid, &code, 3000);
    size_t n = strlen(out);
    for (;;) {
        int r = nema_tty_output_read(t, out + n, cap - n - 1, 100);
        if (r <= 0) break;
        n += (size_t)r;
        out[n] = 0;
    }
    nema_tty_destroy(t);
    return (int)n;
}

#define EDIT(keys, out, cap) edit((keys), sizeof(keys) - 1, (out), (cap))

TEST(plain_line_and_enter) {
    char out[512];
    EDIT("hello\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[hello]") != NULL);
    CHECK(strstr(out, "[eof]") != NULL);
}

TEST(backspace_and_cursor_keys) {
    char out[512];
    EDIT("helx\x7f" "lo\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[hello]") != NULL);
    EDIT("ac\x1b[Db\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[abc]") != NULL); /* left arrow, then insert before c */
    EDIT("ac\x01x\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[xac]") != NULL); /* Ctrl-A goes to the start */
    EDIT("abc\x02\x02X\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[aXbc]") != NULL); /* Ctrl-B moves left */
    EDIT("abc\x1b[H" "X\x1b[F" "Y\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[XabcY]") != NULL); /* Home and End */
}

TEST(delete_key_and_kill_keys) {
    char out[512];
    EDIT("abc\x1b[D\x1b[3~\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[ab]") != NULL); /* forward delete removes the character under the cursor */
    EDIT("abcdef\x1b[D\x1b[D\x1b[D\x0b\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[abc]") != NULL); /* Ctrl-K kills to end of line */
    EDIT("abcdef\x1b[D\x1b[D\x15\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[ef]") != NULL); /* Ctrl-U kills to start of line */
    EDIT("one two three\x17\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[one two ]") != NULL); /* Ctrl-W kills the word before the cursor */
}

TEST(ctrl_d_deletes_or_ends) {
    char out[512];
    EDIT("abc\x01\x04\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[bc]") != NULL); /* Ctrl-D on a non-empty line deletes forward */
    EDIT("\x04", out, sizeof(out));
    CHECK(strstr(out, "[eof]") != NULL); /* on an empty line it ends input */
}

TEST(ctrl_c_drops_the_line) {
    char out[512];
    EDIT("abc\x03" "x\r\x04", out, sizeof(out));
    CHECK(strstr(out, "^C") != NULL);
    CHECK(strstr(out, "[]") != NULL);
    CHECK(strstr(out, "[x]") != NULL);
}

TEST(history_up_down_and_saved_line) {
    char out[512];
    EDIT("one\rtwo\r\x1b[A\r\x1b[A\x1b[A\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[two]") != NULL);
    CHECK(strstr(out, "[one]") != NULL); /* stops at the oldest entry */
    EDIT("one\rpar\x1b[A\x1b[B\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[one]") != NULL);
    CHECK(strstr(out, "[par]") != NULL); /* Down restores the line being typed */
    EDIT("one\rtwo\r\x10\x0e\x0e" "x\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[x]") != NULL);   /* Ctrl-P / Ctrl-N behave like the arrows */
}

TEST(tab_completes_a_unique_file_with_a_space) {
    char out[512];
    EDIT("cat /cmpl/be\t\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[cat /cmpl/beta.txt ]") != NULL);
}

TEST(tab_completes_a_directory_without_a_space) {
    char out[512];
    EDIT("cat /cmpl/su\t\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[cat /cmpl/sub/]") != NULL);
}

TEST(tab_completes_the_common_prefix) {
    char out[512];
    EDIT("cat /cmpl/al\t\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[cat /cmpl/alp]") != NULL); /* alpha.txt and alpine.txt share "alp" */
}

TEST(tab_completes_commands) {
    char out[512];
    EDIT("echo\t\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[echox ]") != NULL);
    EDIT("unal\t\r\x04", out, sizeof(out));
    CHECK(strstr(out, "[unalias ]") != NULL); /* a builtin */
}

TEST(tab_without_match_beeps_and_second_tab_lists) {
    char out[512];
    EDIT("zz\t\r\x04", out, sizeof(out));
    CHECK(strstr(out, "\a") != NULL);
    CHECK(strstr(out, "[zz]") != NULL);
    EDIT("ex\t\t\r\x04", out, sizeof(out));
    CHECK(strstr(out, "exit") != NULL && strstr(out, "export") != NULL); /* candidates listed */
    CHECK(strstr(out, "[ex]") != NULL);
}

static void mkfile(const char *path) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE, &f) == NEMA_OK) nema_obj_unref(f);
}

TEST(a_long_command_still_lets_the_history_be_written) {
    pal_history_reset_for_test("/hist", NULL);
    nema_tty_t *t;
    nema_tty_create(&t, 80, 24, OW);
    nema_spawn_t sp = {.name = "slowcmd", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    nema_proc_spawn(&sp, &pid);
    nema_tty_set_foreground(t, pid);
    char out[512] = "";
    until_prompt(t, out, sizeof(out));
    nema_tty_input(t, "long-one\r", 9);
    /* the command's 2.3 s, then the second prompt: the overdue write happens at once (it used to spin there forever) */
    nema_stat_t st;
    bool written = false;
    for (int i = 0; i < 60 && !written; i++) {
        nema_sleep_ms(100);
        written = nema_fs_stat("/hist", &st) == NEMA_OK && st.size > 0;
    }
    CHECK(written);
    nema_tty_input(t, "\x04", 1);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 3000) == NEMA_OK && code == 0);
    nema_tty_destroy(t);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("edittest");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(512 * 1024));
    nema_fs_mkdir("/cmpl");
    nema_fs_mkdir("/cmpl/sub");
    mkfile("/cmpl/alpha.txt");
    mkfile("/cmpl/alpine.txt");
    mkfile("/cmpl/beta.txt");
    RUN(plain_line_and_enter);
    RUN(a_long_command_still_lets_the_history_be_written);
    RUN(backspace_and_cursor_keys);
    RUN(delete_key_and_kill_keys);
    RUN(ctrl_d_deletes_or_ends);
    RUN(ctrl_c_drops_the_line);
    RUN(history_up_down_and_saved_line);
    RUN(tab_completes_a_unique_file_with_a_space);
    RUN(tab_completes_a_directory_without_a_space);
    RUN(tab_completes_the_common_prefix);
    RUN(tab_completes_commands);
    RUN(tab_without_match_beeps_and_second_tab_lists);
    return TEST_DONE();
}
