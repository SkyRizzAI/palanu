#include <string.h>

#include "nema/nema.h"
#include "test.h"

#define PORT 29417 /* below 30000: out of the other tests' random ports and the system's ephemeral range */

TEST(loopback_echo_through_objects) {
    nema_obj_t *lst, *cli, *srv;
    CHECK(nema_net_listen(PORT, &lst) == NEMA_OK);
    CHECK(nema_net_connect("127.0.0.1", PORT, 2000, &cli) == NEMA_OK);
    CHECK(nema_net_accept(lst, 2000, &srv) == NEMA_OK);
    CHECK_EQ(nema_obj_write(cli, "ping", 4, 1000), 4);
    char b[16] = {0};
    CHECK_EQ(nema_obj_read(srv, b, sizeof(b), 2000), 4);
    CHECK(strcmp(b, "ping") == 0);
    CHECK_EQ(nema_obj_write(srv, "pong!", 5, 1000), 5);
    CHECK_EQ(nema_obj_read(cli, b, sizeof(b), 2000), 5);
    nema_obj_unref(srv);
    CHECK_EQ(nema_obj_read(cli, b, sizeof(b), 2000), 0); /* peer closed: end of stream */
    nema_obj_unref(cli);
    nema_obj_unref(lst);
}

static void delayed_send(void *arg) {
    nema_port_sleep_ms(80);
    nema_obj_write((nema_obj_t *)arg, "late", 4, 1000);
}

TEST(poll_waits_on_socket_and_pipe_together) {
    nema_obj_t *lst, *cli, *srv, *pr, *pw;
    nema_net_listen(PORT + 1, &lst);
    nema_net_connect("127.0.0.1", PORT + 1, 2000, &cli);
    nema_net_accept(lst, 2000, &srv);
    nema_pipe_create(16, NEMA_MEM_ANY, 0, &pr, &pw);
    nema_poll_obj_t fds[2] = {{srv, NEMA_POLL_IN, 0}, {pr, NEMA_POLL_IN, 0}};
    CHECK_EQ(nema_poll_objs(fds, 2, 50), 0); /* nothing yet: times out */
    nema_task_desc_t d = {.name = "snd", .fn = delayed_send, .arg = cli, .stack = 4096, .role = NEMA_TASK_SYSTEM};
    nema_task_create(&d);
    int r = nema_poll_objs(fds, 2, 2000);
    CHECK_EQ(r, 1);
    CHECK(fds[0].revents & NEMA_POLL_IN);
    CHECK(!(fds[1].revents & NEMA_POLL_IN));
    char b[8];
    CHECK_EQ(nema_obj_read(srv, b, sizeof(b), 0), 4);
    nema_obj_write(pw, "x", 1, 0); /* now the pipe side wakes it */
    r = nema_poll_objs(fds, 2, 2000);
    CHECK_EQ(r, 1);
    CHECK(fds[1].revents & NEMA_POLL_IN);
    nema_port_sleep_ms(120);
    nema_obj_unref(pr), nema_obj_unref(pw), nema_obj_unref(srv), nema_obj_unref(cli), nema_obj_unref(lst);
}

TEST(connect_failures_are_reported) {
    nema_obj_t *o;
    CHECK(nema_net_connect("127.0.0.1", 1, 1500, &o) != NEMA_OK); /* nothing listens there */
    char ip[16];
    CHECK(nema_net_resolve("127.0.0.1", ip, sizeof(ip)) == NEMA_OK);
    CHECK(strcmp(ip, "127.0.0.1") == 0);
}

static uint64_t g_sent_us;

static void timed_send(void *arg) {
    nema_port_sleep_ms(100);
    g_sent_us = nema_port_time_us();
    nema_obj_write((nema_obj_t *)arg, "tick", 4, 1000);
}

/* A waiter that arms an idle pump gets the data within 20 ms of the send. */
TEST(delivery_latency_with_idle_pump) {
    nema_obj_t *lst, *cli, *srv;
    CHECK(nema_net_listen(PORT + 2, &lst) == NEMA_OK);
    CHECK(nema_net_connect("127.0.0.1", PORT + 2, 2000, &cli) == NEMA_OK);
    CHECK(nema_net_accept(lst, 2000, &srv) == NEMA_OK);
    nema_port_sleep_ms(200); /* the pump has nothing armed and is asleep */
    nema_task_desc_t d = {.name = "tick", .fn = timed_send, .arg = cli, .stack = 4096, .role = NEMA_TASK_SYSTEM};
    nema_task_create(&d);
    nema_poll_obj_t fds[1] = {{srv, NEMA_POLL_IN, 0}};
    CHECK_EQ(nema_poll_objs(fds, 1, 2000), 1);
    uint64_t lat = nema_port_time_us() - g_sent_us;
    CHECK(lat < 20000);
    char b[8] = {0};
    CHECK_EQ(nema_obj_read(srv, b, sizeof(b), 0), 4);
    nema_port_sleep_ms(120);
    nema_obj_unref(srv), nema_obj_unref(cli), nema_obj_unref(lst);
}

/* Idle means idle: the pump does not wake on its own, and an unwaited socket with unread data does not spin it. */
TEST(idle_pump_does_not_wake) {
    nema_obj_t *lst, *cli, *srv;
    CHECK(nema_net_listen(PORT + 3, &lst) == NEMA_OK);
    CHECK(nema_net_connect("127.0.0.1", PORT + 3, 2000, &cli) == NEMA_OK);
    CHECK(nema_net_accept(lst, 2000, &srv) == NEMA_OK);
    CHECK_EQ(nema_obj_write(cli, "unread", 6, 1000), 6);
    nema_port_sleep_ms(100);
    uint32_t before = nema_net_pump_wakeups();
    nema_port_sleep_ms(500);
    uint32_t after = nema_net_pump_wakeups();
    CHECK(after - before <= 2);
    char b[8] = {0};
    CHECK_EQ(nema_obj_read(srv, b, sizeof(b), 0), 6); /* the data was still there */
    nema_obj_unref(srv), nema_obj_unref(cli), nema_obj_unref(lst);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_net_init();
    RUN(loopback_echo_through_objects);
    RUN(poll_waits_on_socket_and_pipe_together);
    RUN(connect_failures_are_reported);
    RUN(delivery_latency_with_idle_pump);
    RUN(idle_pump_does_not_wake);
    return TEST_DONE();
}
