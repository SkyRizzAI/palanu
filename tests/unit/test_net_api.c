/* The net module of the palanu API, through the psys_api_* functions as a process runs them: connect with a
 * timeout, send and receive, refused and unresolvable names, DNS of localhost, non-blocking reads, accept with a timeout,
 * and a kill that ends a process blocked in connect or in a name lookup. Each scenario is a native app process (argv[1]
 * the scenario, argv[2] a port). Its exit status is the number of failed checks, or the scenario's own status. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../system/src/api_ctx.h"
#include "../../system/src/api_impl.h"
#include "nema/nema.h"
#include "pal/papp.h"
#include "test.h"

/* Each run takes its own block of 100 ports, so two runs at once (or another test) do not take each other's ports. */
static int g_base_port = 39420;
#define BASE_PORT g_base_port
#define CLOSED_PORT (g_base_port + 79) /* nothing listens here */

static int g_bad;
#define AC(cond)                                                              \
    do {                                                                      \
        if (!(cond)) {                                                        \
            g_bad++;                                                          \
            printf("    app check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                     \
    } while (0)

static int32_t port_of(const char *s) { return (int32_t)strtol(s, NULL, 10); }

static void sc_connect_echo(api_ctx_t *c, int32_t port) {
    int32_t fd = psys_api_tcp_connect(c, "127.0.0.1", port, 2000);
    AC(fd >= 0);
    if (fd < 0) return;
    AC(psys_api_write(c, fd, "ping", 4) == 4);
    char b[8] = {0};
    AC(psys_api_read(c, fd, b, sizeof(b)) == 4);
    AC(strcmp(b, "ping") == 0);
    AC(psys_api_shutdown(c, fd, 1) == 0); /* no more writes: the echo side sees the end */
    AC(psys_api_shutdown(c, 1, 2) == NEMA_ERR_BADF); /* stdout is not the app's socket */
    AC(psys_api_close(c, fd) == 0);
}

static void sc_refused(api_ctx_t *c, int32_t port) {
    AC(psys_api_tcp_connect(c, "127.0.0.1", port, 2000) == NEMA_ERR_REFUSED);
    AC(psys_api_tcp_connect(c, "127.0.0.1", 0, 2000) == NEMA_ERR_INVAL);
}

static void sc_dns(api_ctx_t *c) {
    uint8_t ip[4] = {0};
    AC(psys_api_dns_resolve(c, "localhost", ip, sizeof(ip), 2000) == 0);
    AC(ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1);
    AC(psys_api_dns_resolve(c, "localhost", ip, 3, 2000) == NEMA_ERR_INVAL); /* four bytes needed */
    uint64_t t0 = nema_port_time_us();
    int32_t r = psys_api_dns_resolve(c, "no-such-host.invalid", ip, sizeof(ip), 300);
    uint64_t took = nema_port_time_us() - t0;
    AC(r < 0); /* unknown, or still looking when the timeout ended */
    AC(took < 2000000u);
}

static void sc_nonblock(api_ctx_t *c, int32_t port) {
    int32_t fd = psys_api_tcp_connect(c, "127.0.0.1", port, 2000);
    AC(fd >= 0);
    if (fd < 0) return;
    AC(psys_api_set_nonblock(c, fd, 1) == 0);
    char b[8];
    /* nothing has been sent: the read answers at once, with AGAIN. (The palanu read import loops on AGAIN; see
     * docs/API.md, pal_set_nonblock.) */
    AC(nema_read_timeout(fd, b, sizeof(b), -1) == NEMA_ERR_AGAIN);
    AC(psys_api_set_nonblock(c, fd, 0) == 0);
    AC(psys_api_close(c, fd) == 0);
}

static void sc_accept_timeout(api_ctx_t *c, int32_t port) {
    int32_t l = psys_api_tcp_listen(c, port, 4);
    AC(l >= 0);
    if (l < 0) return;
    AC(psys_api_tcp_accept(c, l, 100) == NEMA_ERR_TIMEOUT);
    AC(psys_api_tcp_accept(c, 1, 100) == NEMA_ERR_BADF); /* stdout is not a listener */
    AC(psys_api_close(c, l) == 0);
}

static void sc_accept_one(api_ctx_t *c, int32_t port) {
    int32_t l = psys_api_tcp_listen(c, port, 4);
    AC(l >= 0);
    if (l < 0) return;
    int32_t fd = psys_api_tcp_accept(c, l, 5000);
    AC(fd >= 0);
    if (fd >= 0) {
        char b[8] = {0};
        AC(psys_api_read(c, fd, b, 4) == 4);
        AC(psys_api_write(c, fd, b, 4) == 4);
        psys_api_close(c, fd);
    }
    psys_api_close(c, l);
}

static void sc_net_info(api_ctx_t *c) {
    uint8_t b[12] = {0};
    AC(psys_api_net_info(c, b, sizeof(b)) == 0);
    AC(b[0] <= 1);
    AC(b[1] == 0 && b[2] == 0 && b[3] == 0);
    if (!b[0]) AC(b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0);
    AC(psys_api_net_info(c, b, 11) == NEMA_ERR_INVAL);
}

static void sc_block_connect(api_ctx_t *c, int32_t port) {
    /* the listener's backlog is full (the test filled it): the connect waits for the timeout */
    AC(psys_api_tcp_connect(c, "127.0.0.1", port, 15000) < 0);
}

static void sc_block_resolve(api_ctx_t *c) {
    uint8_t ip[4];
    (void)psys_api_dns_resolve(c, "kill-me.invalid", ip, sizeof(ip), 15000);
}

static const struct {
    const char *name;
} SCENARIOS[] = {{"connect_echo"}, {"refused"},     {"dns"},          {"nonblock"},      {"accept_timeout"},
                 {"accept_one"},   {"net_info"},    {"block_connect"}, {"block_resolve"}, {"http_fetch"}};

/* pal_http_fetch against the local answer below: status, full length, the body that fits, the headers sent, bad arguments. */
static char g_http_req[512];
static void sc_http_fetch(api_ctx_t *c, int32_t port) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", (int)port);
    uint8_t out[16];
    int32_t n = psys_api_http_fetch(c, "GET", url, "X-A: 1\nX-B: 2", NULL, 0, out, sizeof(out), 3000);
    AC(n == 8 + 8); /* the body is 12 bytes; 8 fit after the head */
    int32_t st = out[0] | out[1] << 8 | out[2] << 16 | out[3] << 24, total = out[4] | out[5] << 8 | out[6] << 16 | out[7] << 24;
    AC(st == 200);
    AC(total == 12);
    AC(!memcmp(out + 8, "hello wo", 8));
    AC(strstr(g_http_req, "X-A: 1\r\nX-B: 2\r\n") != NULL);
    AC(psys_api_http_fetch(c, "BREW", url, "", NULL, 0, out, sizeof(out), 1000) == NEMA_ERR_INVAL);
    AC(psys_api_http_fetch(c, "GET", url, "", NULL, 0, out, 7, 1000) == NEMA_ERR_INVAL);
    AC(psys_api_http_fetch(c, "GET", "ftp://x/", "", NULL, 0, out, sizeof(out), 1000) == NEMA_ERR_INVAL);
}

static int netapp_main(int argc, char **argv) {
    if (argc < 2) return 99;
    pal_papp_t app;
    memset(&app, 0, sizeof(app));
    snprintf(app.id, sizeof(app.id), "netapp");
    app.api = 2;
    app.perms = 2 | 16; /* net and net.listen */
    api_ctx_t *c = api_ctx_new(&app, argc, argv);
    if (!c) return 98;
    int32_t port = argc > 2 ? port_of(argv[2]) : 0;
    g_bad = 0;
    int found = 0;
    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); i++) {
        if (strcmp(argv[1], SCENARIOS[i].name) != 0) continue;
        found = 1;
        if (strcmp(argv[1], "connect_echo") == 0) sc_connect_echo(c, port);
        else if (strcmp(argv[1], "refused") == 0) sc_refused(c, port);
        else if (strcmp(argv[1], "dns") == 0) sc_dns(c);
        else if (strcmp(argv[1], "nonblock") == 0) sc_nonblock(c, port);
        else if (strcmp(argv[1], "accept_timeout") == 0) sc_accept_timeout(c, port);
        else if (strcmp(argv[1], "accept_one") == 0) sc_accept_one(c, port);
        else if (strcmp(argv[1], "net_info") == 0) sc_net_info(c);
        else if (strcmp(argv[1], "block_connect") == 0) sc_block_connect(c, port);
        else if (strcmp(argv[1], "block_resolve") == 0) sc_block_resolve(c);
        else if (strcmp(argv[1], "http_fetch") == 0) sc_http_fetch(c, port);
        break;
    }
    api_ctx_free(c);
    return found ? g_bad : 97;
}

static const nema_app_t NETAPP = {"netapp", "net module scenarios (tests)", netapp_main, 0, NEMA_APP_HIDDEN};

/* Spawns a scenario; returns its process, or -1 on failure. */
static nema_pid_t spawn(const char *scen, const char *port) {
    const char *av[] = {"netapp", scen, port};
    nema_spawn_t sp = {.name = "netapp", .argc = port ? 3 : 2, .argv = av};
    nema_pid_t pid;
    return nema_proc_spawn(&sp, &pid) == NEMA_OK ? pid : -1;
}

static int run(const char *scen, const char *port) {
    nema_pid_t pid = spawn(scen, port);
    if (pid < 0) return -1000;
    int code = -1;
    if (nema_proc_wait(pid, &code, 15000) != NEMA_OK) return -1001;
    return code;
}

/* A server for connect_echo: accepts one client, echoes until it ends. */
static void echo_once(void *arg) {
    nema_obj_t *l = (nema_obj_t *)arg;
    nema_obj_t *s = NULL;
    if (nema_net_accept(l, 5000, &s) == NEMA_OK) {
        char b[64];
        int n;
        while ((n = nema_obj_read(s, b, sizeof(b), 3000)) > 0) nema_obj_write(s, b, (size_t)n, 3000);
        nema_obj_unref(s);
    }
    nema_obj_unref(l);
}

TEST(connect_send_receive_and_shutdown_through_handles) {
    nema_obj_t *lst = NULL;
    CHECK(nema_net_listen(BASE_PORT, &lst) == NEMA_OK);
    if (!lst) return; /* the server task would unref a listener that does not exist */
    nema_task_desc_t d = {.name = "echo", .fn = echo_once, .arg = lst, .stack = 6144, .role = NEMA_TASK_SYSTEM};
    CHECK(nema_task_create(&d) == NEMA_OK);
    char p[8];
    snprintf(p, sizeof(p), "%d", BASE_PORT);
    CHECK_EQ(run("connect_echo", p), 0);
}

/* A server for http_fetch: one request, one answer of 12 bytes. */
static void http_once(void *arg) {
    nema_obj_t *l = (nema_obj_t *)arg;
    nema_obj_t *s = NULL;
    if (nema_net_accept(l, 5000, &s) == NEMA_OK) {
        size_t n = 0;
        int r;
        while (n + 1 < sizeof(g_http_req) && (r = nema_obj_read(s, g_http_req + n, sizeof(g_http_req) - 1 - n, 3000)) > 0) {
            n += (size_t)r;
            g_http_req[n] = 0;
            if (strstr(g_http_req, "\r\n\r\n")) break;
        }
        const char *a = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nhello world!";
        nema_obj_write(s, a, strlen(a), 3000);
        nema_obj_unref(s);
    }
    nema_obj_unref(l);
}

TEST(http_fetch_returns_the_status_length_and_body) {
    nema_obj_t *lst = NULL;
    CHECK(nema_net_listen(BASE_PORT + 20, &lst) == NEMA_OK);
    if (!lst) return;
    nema_task_desc_t d = {.name = "httpd", .fn = http_once, .arg = lst, .stack = 6144, .role = NEMA_TASK_SYSTEM};
    CHECK(nema_task_create(&d) == NEMA_OK);
    char p[8];
    snprintf(p, sizeof(p), "%d", BASE_PORT + 20);
    CHECK_EQ(run("http_fetch", p), 0);
}

TEST(connect_to_a_closed_port_is_refused) {
    char p[8];
    snprintf(p, sizeof(p), "%d", CLOSED_PORT);
    CHECK_EQ(run("refused", p), 0);
}

TEST(dns_resolves_localhost_and_times_out_on_an_unknown_name) { CHECK_EQ(run("dns", NULL), 0); }

TEST(a_non_blocking_read_with_no_data_returns_eagain) {
    nema_obj_t *lst = NULL;
    CHECK(nema_net_listen(BASE_PORT + 1, &lst) == NEMA_OK); /* nobody accepts: the connection waits in the backlog */
    if (!lst) return;
    char p[8];
    snprintf(p, sizeof(p), "%d", BASE_PORT + 1);
    CHECK_EQ(run("nonblock", p), 0);
    nema_obj_unref(lst);
}

TEST(accept_times_out_and_a_client_is_accepted) {
    char p[8];
    snprintf(p, sizeof(p), "%d", BASE_PORT + 2);
    CHECK_EQ(run("accept_timeout", p), 0);
    /* a real client: the app listens, the test connects (retrying until it listens) and sends four bytes */
    snprintf(p, sizeof(p), "%d", BASE_PORT + 3);
    nema_pid_t pid = spawn("accept_one", p);
    CHECK(pid >= 0);
    nema_obj_t *cli = NULL;
    for (int i = 0; i < 100 && nema_net_connect("127.0.0.1", BASE_PORT + 3, 500, &cli) != NEMA_OK; i++) nema_port_sleep_ms(20);
    CHECK(cli != NULL);
    if (cli) {
        CHECK_EQ(nema_obj_write(cli, "hi!!", 4, 2000), 4);
        char b[8] = {0};
        CHECK_EQ(nema_obj_read(cli, b, sizeof(b), 3000), 4);
        CHECK(strcmp(b, "hi!!") == 0);
        nema_obj_unref(cli);
    }
    int code = -1;
    CHECK(nema_proc_wait(pid, &code, 10000) == NEMA_OK);
    CHECK_EQ(code, 0);
}

TEST(net_info_has_the_record_shape) { CHECK_EQ(run("net_info", NULL), 0); }

/* KILL ends a process blocked in its call within a second (cancellation, docs/adr/0003). Returns whether the process was
 * still running when the kill was sent: a call that had already returned is not a blocked one. */
static bool kill_blocked(const char *scen, const char *port) {
    nema_pid_t pid = spawn(scen, port);
    CHECK(pid >= 0);
    if (pid < 0) return false;
    nema_port_sleep_ms(150);
    int code = -1;
    bool running = nema_proc_wait(pid, &code, 0) != NEMA_OK; /* a finished process is reaped by this probe */
    if (!running) return false;
    CHECK(nema_proc_signal(pid, NEMA_SIG_KILL) == NEMA_OK);
    uint64_t t0 = nema_port_time_us();
    CHECK(nema_proc_wait(pid, &code, 5000) == NEMA_OK);
    CHECK(nema_port_time_us() - t0 < 1000000u);
    return true;
}

/* Deterministic: the listener has a backlog of one and is full, so the client's SYN is never answered. */
TEST(kill_ends_a_process_blocked_in_connect) {
    nema_obj_t *l = NULL, *fill[4] = {NULL, NULL, NULL, NULL};
    CHECK(nema_net_listen_bl(BASE_PORT + 4, 1, &l) == NEMA_OK);
    if (!l) return;
    for (int i = 0; i < 4; i++) nema_net_connect("127.0.0.1", BASE_PORT + 4, 300, &fill[i]);
    char p[8];
    snprintf(p, sizeof(p), "%d", BASE_PORT + 4);
    CHECK(kill_blocked("block_connect", p)); /* it was blocked: still running at the kill */
    for (int i = 0; i < 4; i++) nema_obj_unref(fill[i]);
    nema_obj_unref(l);
}

/* Not deterministic offline: a name that the resolver answers at once is not blocked. The test reports whether the
 * lookup was still running at the kill (it is then the case the kill has to end); the process must end either way. */
TEST(kill_ends_a_process_in_a_name_lookup_if_it_is_still_in_one) {
    bool blocked = kill_blocked("block_resolve", NULL);
    printf("    (name lookup still running at the kill: %s)\n", blocked ? "yes" : "no, it had already returned");
}

/* The ports the tests listen on are free (an earlier run's connections may still hold some). */
static bool ports_free(void) {
    static const int used[] = {0, 1, 2, 3, 4, 20};
    for (size_t i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
        nema_obj_t *l = NULL;
        if (nema_net_listen(g_base_port + used[i], &l) != NEMA_OK) return false;
        nema_obj_unref(l);
    }
    return true;
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_net_init();
    g_base_port = 30000 + (int)(nema_port_time_us() % 300u) * 100;
    for (int tries = 0; tries < 20 && !ports_free(); tries++) g_base_port = 30000 + (g_base_port - 30000 + 700) % 30000;
    nema_app_register(&NETAPP);
    RUN(connect_send_receive_and_shutdown_through_handles);
    RUN(connect_to_a_closed_port_is_refused);
    RUN(http_fetch_returns_the_status_length_and_body);
    RUN(dns_resolves_localhost_and_times_out_on_an_unknown_name);
    RUN(a_non_blocking_read_with_no_data_returns_eagain);
    RUN(accept_times_out_and_a_client_is_accepted);
    RUN(net_info_has_the_record_shape);
    RUN(kill_ends_a_process_blocked_in_connect);
    RUN(kill_ends_a_process_in_a_name_lookup_if_it_is_still_in_one);
    return TEST_DONE();
}
