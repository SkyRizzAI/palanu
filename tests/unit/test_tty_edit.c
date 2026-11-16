/* The tty edit-state side channel (nema/src/tty.c): the line the shell is editing, for the terminal side. Set, get, clear,
 * the cut at 256 bytes and at a UTF-8 boundary, the cursor clamp, the clear on raw mode and on submit, and a thread smoke
 * test: a reader never sees a copy mixed from two writes. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "nema/tty.h"
#include "test.h"

static nema_owner_t OW;

static nema_tty_t *make(void) {
    nema_tty_t *t = NULL;
    nema_tty_create(&t, 80, 24, OW);
    return t;
}

TEST(set_get_clear) {
    nema_tty_t *t = make();
    char out[64];
    size_t cur = 99;
    CHECK_EQ(nema_tty_get_edit(t, out, sizeof(out), &cur), -1); /* nothing is being edited */
    nema_tty_set_edit(t, "ls -la", 6, 3);
    int n = nema_tty_get_edit(t, out, sizeof(out), &cur);
    CHECK_EQ(n, 6);
    CHECK(memcmp(out, "ls -la", 6) == 0);
    CHECK_EQ(out[6], 0);
    CHECK_EQ(cur, 3);
    nema_tty_clear_edit(t);
    CHECK_EQ(nema_tty_get_edit(t, out, sizeof(out), &cur), -1);
    nema_tty_destroy(t);
}

TEST(cursor_is_clamped_and_copy_is_cut) {
    nema_tty_t *t = make();
    char out[300];
    size_t cur = 0;
    nema_tty_set_edit(t, "abc", 3, 40); /* cursor past the end */
    CHECK_EQ(nema_tty_get_edit(t, out, sizeof(out), &cur), 3);
    CHECK_EQ(cur, 3);
    char big[400];
    memset(big, 'x', sizeof(big));
    nema_tty_set_edit(t, big, sizeof(big), 300);
    int n = nema_tty_get_edit(t, out, sizeof(out), &cur);
    CHECK_EQ(n, NEMA_TTY_EDIT_MAX);
    CHECK_EQ(cur, NEMA_TTY_EDIT_MAX);
    /* a short output buffer takes what fits */
    char small[5];
    CHECK_EQ(nema_tty_get_edit(t, small, sizeof(small), NULL), 4);
    CHECK(memcmp(small, "xxxx", 4) == 0);
    nema_tty_destroy(t);
}

TEST(cut_never_splits_a_character) {
    nema_tty_t *t = make();
    char text[300];
    size_t len = 0;
    while (len + 3 <= 258) { /* euro signs: three bytes each */
        text[len++] = (char)0xE2;
        text[len++] = (char)0x82;
        text[len++] = (char)0xAC;
    }
    nema_tty_set_edit(t, text, len, 0);
    char out[300];
    int n = nema_tty_get_edit(t, out, sizeof(out), NULL);
    CHECK_EQ(n % 3, 0);
    CHECK(n <= NEMA_TTY_EDIT_MAX);
    nema_tty_destroy(t);
}

/* The process end's ioctl, as the shell reaches it through nema_ioctl(0, ...). */
static nema_err_t proc_ioctl(nema_tty_t *t, uint32_t req, void *arg) {
    nema_obj_t *o = nema_tty_proc_obj(t);
    return o->ops->ioctl(o, req, arg);
}

TEST(ioctl_publish_clear_and_raw_mode) {
    nema_tty_t *t = make();
    /* the process side: publish through the ioctl the shell uses */
    nema_edit_t e = {"hi", 2, 1};
    CHECK_EQ(proc_ioctl(t, NEMA_TIOC_SETEDIT, &e), NEMA_OK);
    char out[16];
    size_t cur = 0;
    CHECK_EQ(nema_tty_get_edit(t, out, sizeof(out), &cur), 2);
    CHECK_EQ(cur, 1);
    /* a program that takes the terminal in raw mode: no line is being edited */
    uint32_t mode = NEMA_TTY_RAW;
    CHECK_EQ(proc_ioctl(t, NEMA_TIOC_SETMODE, &mode), NEMA_OK);
    CHECK_EQ(nema_tty_get_edit(t, out, sizeof(out), &cur), -1);
    /* clear request */
    nema_edit_t e2 = {"x", 1, 0};
    proc_ioctl(t, NEMA_TIOC_SETEDIT, &e2);
    CHECK_EQ(proc_ioctl(t, NEMA_TIOC_CLREDIT, NULL), NEMA_OK);
    CHECK_EQ(nema_tty_get_edit(t, out, sizeof(out), &cur), -1);
    nema_tty_destroy(t);
}

TEST(legend_text_is_kept_counted_and_cleared) {
    nema_tty_t *t = make();
    char out[NEMA_TTY_LEGEND_MAX];
    uint32_t g0 = nema_tty_get_legend(t, out, sizeof(out));
    CHECK_EQ(out[0], 0); /* nothing published */
    CHECK_EQ(proc_ioctl(t, NEMA_TIOC_SETLEGEND, (void *)"ok=Jump;back=Quit"), NEMA_OK);
    uint32_t g1 = nema_tty_get_legend(t, out, sizeof(out));
    CHECK(g1 != g0);
    CHECK(strcmp(out, "ok=Jump;back=Quit") == 0);
    CHECK_EQ(nema_tty_get_legend(t, out, sizeof(out)), g1); /* reading does not change the counter */
    char small[4];
    nema_tty_get_legend(t, small, sizeof(small));
    CHECK(strcmp(small, "ok=") == 0);
    char big[NEMA_TTY_LEGEND_MAX + 1];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK_EQ(proc_ioctl(t, NEMA_TIOC_SETLEGEND, big), NEMA_ERR_INVAL); /* too long: refused, the old text stays */
    CHECK_EQ(nema_tty_get_legend(t, out, sizeof(out)), g1);
    CHECK_EQ(proc_ioctl(t, NEMA_TIOC_SETLEGEND, NULL), NEMA_OK);
    CHECK(nema_tty_get_legend(t, out, sizeof(out)) != g1);
    CHECK_EQ(out[0], 0);
    nema_tty_destroy(t);
}

/* ---- thread smoke: the reader gets one whole value or the other ---- */
static nema_tty_t *g_t;
static volatile bool g_stop;
static void *writer(void *arg) {
    (void)arg;
    char a[200], b[200];
    memset(a, 'A', sizeof(a));
    memset(b, 'B', sizeof(b));
    for (int i = 0; i < 4000; i++) nema_tty_set_edit(g_t, (i & 1) ? a : b, sizeof(a), 10);
    g_stop = true;
    return NULL;
}
TEST(thread_smoke_reads_are_whole) {
    g_t = make();
    g_stop = false;
    pthread_t w;
    pthread_create(&w, NULL, writer, NULL);
    int bad = 0, reads = 0;
    char out[300];
    while (!g_stop) {
        int n = nema_tty_get_edit(g_t, out, sizeof(out), NULL);
        if (n <= 0) continue;
        reads++;
        for (int i = 1; i < n; i++)
            if (out[i] != out[0]) {
                bad++;
                break;
            }
    }
    pthread_join(w, NULL);
    CHECK_EQ(bad, 0);
    CHECK(reads > 0);
    nema_tty_destroy(g_t);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("ttyedit");
    RUN(set_get_clear);
    RUN(cursor_is_clamped_and_copy_is_cut);
    RUN(cut_never_splits_a_character);
    RUN(ioctl_publish_clear_and_raw_mode);
    RUN(legend_text_is_kept_counted_and_cleared);
    RUN(thread_smoke_reads_are_whole);
    return TEST_DONE();
}
