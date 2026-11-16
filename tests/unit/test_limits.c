/* Kernel limits: 32 process slots, 16 wait objects, a growable app registry, and unregister. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "test.h"

static nema_owner_t OW;

/* block: waits on stdin until a signal, then returns 130 */
static int app_block(int argc, char **argv) {
    char b[4];
    (void)argc, (void)argv;
    int n = nema_read(0, b, sizeof(b));
    (void)n;
    return nema_interrupted() ? 130 : 0;
}

static int app_noop(int argc, char **argv) {
    (void)argc, (void)argv;
    return 0;
}

static const nema_app_t APPS[] = {
    {"limit-block", "wait for a signal", app_block, 0, 0},
    {"limit-hold", "wait for a signal, then return 130", app_block, 0, 0},
};

TEST(thirty_two_processes_run_and_the_thirty_third_is_refused) {
    nema_obj_t *rd[NEMA_PROC_MAX], *wr[NEMA_PROC_MAX];
    nema_pid_t pid[NEMA_PROC_MAX];
    int started = 0;
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        CHECK(nema_pipe_create(16, NEMA_MEM_ANY, OW, &rd[i], &wr[i]) == NEMA_OK);
        nema_spawn_t sp = {.name = "limit-block", .stdio = {rd[i], NULL, NULL}};
        if (nema_proc_spawn(&sp, &pid[i]) == NEMA_OK) started++;
    }
    CHECK_EQ(started, 32);
    /* the 33rd: the table is full, so the refusal is clean (no slot, no task, no pipe leak in the table) */
    nema_obj_t *r33, *w33;
    CHECK(nema_pipe_create(16, NEMA_MEM_ANY, OW, &r33, &w33) == NEMA_OK);
    nema_spawn_t extra = {.name = "limit-block", .stdio = {r33, NULL, NULL}};
    nema_pid_t extra_pid = -1;
    CHECK_EQ(nema_proc_spawn(&extra, &extra_pid), NEMA_ERR_FULL);
    CHECK_EQ(extra_pid, -1);
    nema_obj_unref(r33);
    nema_obj_unref(w33);
    nema_proc_t list[NEMA_PROC_MAX];
    CHECK_EQ(nema_proc_list(list, NEMA_PROC_MAX), NEMA_PROC_MAX);
    CHECK_EQ(nema_task_count(), 32);
    for (int i = 0; i < NEMA_PROC_MAX; i++) CHECK(nema_proc_signal(pid[i], NEMA_SIG_INT) == NEMA_OK);
    for (int i = 0; i < NEMA_PROC_MAX; i++) {
        int code = -1;
        CHECK(nema_proc_wait(pid[i], &code, 3000) == NEMA_OK);
        CHECK_EQ(code, 130);
        nema_obj_unref(rd[i]);
        nema_obj_unref(wr[i]);
    }
    nema_port_sleep_ms(20);
    CHECK_EQ(nema_task_count(), 0);
}

TEST(wait_objects_limit_is_sixteen) {
    nema_obj_t *r[NEMA_WAIT_OBJS_MAX + 1], *w[NEMA_WAIT_OBJS_MAX + 1];
    for (int i = 0; i < NEMA_WAIT_OBJS_MAX + 1; i++) nema_pipe_create(8, NEMA_MEM_ANY, OW, &r[i], &w[i]);
    /* sixteen entries wait normally: a poll with no ready objects times out */
    nema_poll_obj_t objs[NEMA_WAIT_OBJS_MAX];
    for (int i = 0; i < NEMA_WAIT_OBJS_MAX; i++) {
        objs[i].obj = r[i];
        objs[i].events = NEMA_POLL_IN;
        objs[i].revents = 0;
    }
    CHECK_EQ(nema_poll_objs(objs, NEMA_WAIT_OBJS_MAX, 10), 0);
    /* seventeen is refused, not truncated */
    nema_poll_obj_t more[NEMA_WAIT_OBJS_MAX + 1];
    for (int i = 0; i < NEMA_WAIT_OBJS_MAX + 1; i++) {
        more[i].obj = r[i];
        more[i].events = NEMA_POLL_IN;
        more[i].revents = 0;
    }
    CHECK_EQ(nema_poll_objs(more, NEMA_WAIT_OBJS_MAX + 1, 10), NEMA_ERR_INVAL);
    /* a write end becomes readable on the sixteenth pipe: the wait returns it */
    nema_obj_write(w[NEMA_WAIT_OBJS_MAX - 1], "x", 1, 0);
    CHECK_EQ(nema_poll_objs(objs, NEMA_WAIT_OBJS_MAX, 100), 1);
    CHECK(objs[NEMA_WAIT_OBJS_MAX - 1].revents != 0);
    for (int i = 0; i < NEMA_WAIT_OBJS_MAX + 1; i++) {
        nema_obj_unref(r[i]);
        nema_obj_unref(w[i]);
    }
}

TEST(registry_grows_past_sixty_four_commands) {
    enum { N = 200 };
    static nema_app_t many[N];
    static char names[N][16];
    size_t before = nema_app_count();
    int ok = 0;
    for (int i = 0; i < N; i++) {
        snprintf(names[i], sizeof(names[i]), "grow-%d", i);
        many[i] = (nema_app_t){names[i], "registry growth test", app_noop, 0, 0};
        if (nema_app_register(&many[i]) == NEMA_OK) ok++;
    }
    CHECK_EQ(ok, N);
    CHECK_EQ(nema_app_count(), before + N);
    CHECK(nema_app_find("grow-199") == &many[199]);
    CHECK(nema_app_find("grow-0") == &many[0]);
    CHECK_EQ(nema_app_register(&many[5]), NEMA_ERR_EXIST);
    /* the built-in test commands registered first are still there and still findable */
    CHECK(nema_app_find("limit-block") != NULL);
    /* spawning one of the grown commands works */
    nema_spawn_t sp = {.name = "grow-150"};
    nema_pid_t pid;
    CHECK(nema_proc_spawn(&sp, &pid) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 0);
    for (int i = 0; i < N; i++) CHECK_EQ(nema_app_unregister(names[i]), NEMA_OK);
    CHECK_EQ(nema_app_count(), before);
}

TEST(unregister_removes_the_command_and_refuses_while_it_runs) {
    static nema_app_t tmp = {"limit-tmp", "temporary", app_noop, 0, 0};
    CHECK(nema_app_register(&tmp) == NEMA_OK);
    CHECK(nema_app_find("limit-tmp") == &tmp);
    CHECK_EQ(nema_app_unregister("limit-tmp"), NEMA_OK);
    CHECK(nema_app_find("limit-tmp") == NULL);
    CHECK_EQ(nema_app_unregister("limit-tmp"), NEMA_ERR_NOENT);
    nema_spawn_t gone = {.name = "limit-tmp"};
    nema_pid_t pid;
    CHECK_EQ(nema_proc_spawn(&gone, &pid), NEMA_ERR_NOENT);
    /* registered again, it runs; unregister is refused while it is running */
    CHECK(nema_app_register(&tmp) == NEMA_OK);
    nema_obj_t *r, *w;
    nema_pipe_create(16, NEMA_MEM_ANY, OW, &r, &w);
    nema_spawn_t hold = {.name = "limit-hold", .stdio = {r, NULL, NULL}};
    CHECK(nema_proc_spawn(&hold, &pid) == NEMA_OK);
    nema_port_sleep_ms(30);
    CHECK_EQ(nema_app_unregister("limit-hold"), NEMA_ERR_BUSY);
    CHECK(nema_app_find("limit-hold") != NULL);
    CHECK(nema_proc_signal(pid, NEMA_SIG_INT) == NEMA_OK);
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 2000) == NEMA_OK);
    CHECK_EQ(code, 130);
    CHECK_EQ(nema_app_unregister("limit-hold"), NEMA_OK);
    CHECK(nema_app_find("limit-hold") == NULL);
    CHECK_EQ(nema_app_unregister("limit-tmp"), NEMA_OK);
    nema_obj_unref(r);
    nema_obj_unref(w);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    OW = nema_owner_register("limtest");
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(APPS); i++) nema_app_register(&APPS[i]);
    RUN(thirty_two_processes_run_and_the_thirty_third_is_refused);
    RUN(wait_objects_limit_is_sixteen);
    RUN(registry_grows_past_sixty_four_commands);
    RUN(unregister_removes_the_command_and_refuses_while_it_runs);
    return TEST_DONE();
}
