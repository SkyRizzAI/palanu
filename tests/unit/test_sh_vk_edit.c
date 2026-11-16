/* The shell publishes the line it is editing (apps/sh_edit.c) to the terminal side, where the on-screen keyboard reads it.
 * The editor runs as a real process on a TTY in raw mode; the test types keys and reads the published line back. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "nema/tty.h"
#include "sh.h"
#include "test.h"

static nema_owner_t OW;

static int app_edit(int argc, char **argv) {
    (void)argc, (void)argv;
    static sh_t SH;
    uint32_t mode = NEMA_TTY_RAW;
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    memset(&SH, 0, sizeof(SH));
    SH.owner = nema_self()->owner;
    SH.cols = 80;
    for (;;) {
        char line[SH_LINE_MAX];
        int n = sh_readline(&SH, "> ", line, sizeof(line));
        if (n < 0) return 0;
        char rec[SH_LINE_MAX + 4];
        int m = snprintf(rec, sizeof(rec), "[%s]", line);
        nema_write(1, rec, (size_t)m);
    }
}

static const nema_app_t APPS[] = {{"editvk", "", app_edit, 0, NEMA_APP_HIDDEN}};

/* Wait until the published line matches want and its cursor is want_cur (or the timeout). */
static int wait_edit(nema_tty_t *t, const char *want, size_t want_cur, size_t *cur) {
    char out[SH_LINE_MAX];
    int n = -1;
    for (int i = 0; i < 400; i++) {
        n = nema_tty_get_edit(t, out, sizeof(out), cur);
        if (n >= 0 && strcmp(out, want) == 0 && *cur == want_cur) return n;
        nema_port_sleep_ms(5);
    }
    return n;
}

TEST(shell_publishes_the_line_and_its_cursor) {
    nema_tty_t *t;
    nema_tty_create(&t, 80, 24, OW);
    nema_spawn_t sp = {.name = "editvk", .stdio = {nema_tty_proc_obj(t), nema_tty_proc_obj(t), NULL}};
    nema_pid_t pid;
    nema_proc_spawn(&sp, &pid);
    nema_tty_set_foreground(t, pid);
    char buf[256];
    int r = 0;
    for (int i = 0; i < 400 && r == 0; i++) { /* the editor has drawn its prompt */
        int k = nema_tty_output_read(t, buf, sizeof(buf) - 1, 20);
        if (k > 0 && strstr(buf, "> ")) r = 1;
    }
    CHECK_EQ(r, 1);
    size_t cur = 99;
    /* nothing typed yet: an empty line is published */
    CHECK_EQ(wait_edit(t, "", 0, &cur), 0);
    nema_tty_input(t, "ls", 2);
    CHECK_EQ(wait_edit(t, "ls", 2, &cur), 2);
    nema_tty_input(t, "\x1b[D", 3); /* left arrow */
    CHECK_EQ(wait_edit(t, "ls", 1, &cur), 2);
    CHECK_EQ(cur, 1);
    nema_tty_input(t, "\r", 1); /* submitted: the next prompt starts with an empty line */
    CHECK_EQ(wait_edit(t, "", 0, &cur), 0);
    nema_tty_input(t, "\x04", 1); /* Ctrl-D on the empty line ends the editor: nothing is edited any more */
    int code = -1;
    nema_proc_wait(pid, &code, 3000);
    CHECK_EQ(nema_tty_get_edit(t, buf, sizeof(buf), NULL), -1);
    nema_tty_destroy(t);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("shvk");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    RUN(shell_publishes_the_line_and_its_cursor);
    return TEST_DONE();
}
