/* The HTTP client (pal/http.h) against a small local server task: Content-Length and chunked bodies, a redirect, a POST with
 * its body and headers, HEAD with no body, a stopped transfer, too many redirects, and URL parsing. No internet needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "nema/net.h"
#include "pal/http.h"
#include "test.h"

#define PORT 39530

/* ---- the server: answers `count` connections in turn with the canned answers in `ans` ---- */
static struct {
    nema_obj_t *lst;
    const char *ans[4];
    int count;
    char got[4][1024]; /* each request as received (head and body) */
    volatile int done;
} SV;

static void serve(void *arg) {
    (void)arg;
    for (int i = 0; i < SV.count; i++) {
        nema_obj_t *s = NULL;
        if (nema_net_accept(SV.lst, 5000, &s) != NEMA_OK) break;
        size_t n = 0;
        char *g = SV.got[i];
        long body = -1;
        for (;;) { /* the head up to the blank line, then Content-Length bytes of body */
            int r = nema_obj_read(s, g + n, sizeof(SV.got[i]) - 1 - n, 3000);
            if (r <= 0) break;
            n += (size_t)r;
            g[n] = 0;
            char *end = strstr(g, "\r\n\r\n");
            if (!end) continue;
            if (body < 0) {
                char *cl = strstr(g, "Content-Length: ");
                body = cl ? strtol(cl + 16, NULL, 10) : 0;
            }
            if ((long)(n - (size_t)(end + 4 - g)) >= body) break;
        }
        nema_obj_write(s, SV.ans[i], strlen(SV.ans[i]), 3000);
        nema_obj_unref(s);
    }
    SV.done = 1;
}

static void server(int count, const char *a0, const char *a1) {
    memset(&SV.got, 0, sizeof(SV.got));
    SV.count = count, SV.ans[0] = a0, SV.ans[1] = a1, SV.done = 0;
    nema_task_desc_t d = {.name = "httpd", .fn = serve, .stack = 8192, .role = NEMA_TASK_SYSTEM};
    nema_task_create(&d);
}

static void wait_server(void) {
    for (int i = 0; i < 300 && !SV.done; i++) nema_sleep_ms(10);
}

/* ---- the client side ---- */
static char g_body[256];
static size_t g_len;
static int g_stop_after; /* stop the transfer after this many bytes (0: never) */
static int collect(void *ctx, const void *buf, size_t n) {
    (void)ctx;
    if (g_len + n < sizeof(g_body)) memcpy(g_body + g_len, buf, n);
    g_len += n;
    g_body[g_len < sizeof(g_body) ? g_len : sizeof(g_body) - 1] = 0;
    return g_stop_after && g_len >= (size_t)g_stop_after ? -1 : 0;
}

static nema_err_t get(pal_http_req_t *q, pal_http_resp_t *r) {
    g_len = 0, g_body[0] = 0;
    q->on_data = collect;
    q->timeout_ms = 3000;
    return pal_http_request(q, r);
}

#define URL(path) "http://127.0.0.1:39530" path

TEST(content_length_body_and_headers) {
    server(1, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello", NULL);
    pal_http_req_t q = {.url = URL("/a?x=1")};
    pal_http_resp_t r;
    CHECK_EQ(get(&q, &r), NEMA_OK);
    wait_server();
    CHECK_EQ(r.status, 200);
    CHECK_EQ((int)r.length, 5);
    CHECK(!strcmp(g_body, "hello"));
    CHECK(!strcmp(r.type, "text/plain"));
    CHECK(!strncmp(SV.got[0], "GET /a?x=1 HTTP/1.1\r\n", 21));
    CHECK(strstr(SV.got[0], "Host: 127.0.0.1:39530\r\n") != NULL);
}

TEST(chunked_body_is_joined) {
    server(1, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", NULL);
    pal_http_req_t q = {.url = URL("/c")};
    pal_http_resp_t r;
    CHECK_EQ(get(&q, &r), NEMA_OK);
    wait_server();
    CHECK(!strcmp(g_body, "Wikipedia"));
    CHECK_EQ((int)r.received, 9);
}

TEST(a_redirect_is_followed_to_the_final_url) {
    server(2, "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n\r\n",
           "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone");
    pal_http_req_t q = {.url = URL("/start"), .redirects = 3};
    pal_http_resp_t r;
    CHECK_EQ(get(&q, &r), NEMA_OK);
    wait_server();
    CHECK_EQ(r.status, 200);
    CHECK(!strcmp(g_body, "done"));
    CHECK(!strcmp(r.url, URL("/final")));
    CHECK(!strncmp(SV.got[1], "GET /final ", 11));
}

TEST(a_redirect_is_handed_back_when_not_followed_or_too_many) {
    server(1, "HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1:39530/x\r\nContent-Length: 0\r\n\r\n", NULL);
    pal_http_req_t q = {.url = URL("/old")};
    pal_http_resp_t r;
    CHECK_EQ(get(&q, &r), NEMA_OK);
    wait_server();
    CHECK_EQ(r.status, 301);
    server(2, "HTTP/1.1 302 Found\r\nLocation: /a\r\n\r\n", "HTTP/1.1 302 Found\r\nLocation: /b\r\n\r\n");
    q.redirects = 1;
    CHECK_EQ(get(&q, &r), NEMA_ERR_RANGE);
    wait_server();
}

TEST(post_sends_its_body_and_headers) {
    server(1, "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok", NULL);
    const char *h[] = {"X-Test: yes", NULL};
    pal_http_req_t q = {.url = URL("/submit"), .body = "a=1", .body_len = 3, .headers = h};
    pal_http_resp_t r;
    CHECK_EQ(get(&q, &r), NEMA_OK);
    wait_server();
    CHECK_EQ(r.status, 201);
    CHECK(!strncmp(SV.got[0], "POST /submit HTTP/1.1\r\n", 23));
    CHECK(strstr(SV.got[0], "X-Test: yes\r\n") != NULL);
    CHECK(strstr(SV.got[0], "Content-Length: 3\r\n") != NULL);
    char *end = strstr(SV.got[0], "\r\n\r\n");
    CHECK(end && !strcmp(end + 4, "a=1"));
}

TEST(head_reads_no_body) {
    server(1, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", NULL);
    pal_http_req_t q = {.url = URL("/h"), .method = "HEAD"};
    pal_http_resp_t r;
    uint64_t t0 = nema_port_time_us();
    CHECK_EQ(get(&q, &r), NEMA_OK);
    wait_server();
    CHECK_EQ((int)r.length, 100);
    CHECK_EQ((int)r.received, 0);
    CHECK(nema_port_time_us() - t0 < 2000000u); /* did not wait for 100 bytes that never come */
}

TEST(on_data_can_stop_the_transfer) {
    server(1, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", NULL);
    pal_http_req_t q = {.url = URL("/big")};
    pal_http_resp_t r;
    g_stop_after = 1;
    CHECK_EQ(get(&q, &r), NEMA_ERR_CANCELED);
    g_stop_after = 0;
    wait_server();
}

TEST(urls_are_split_and_bad_ones_refused) {
    bool tls;
    char host[32], path[64];
    uint16_t port;
    CHECK_EQ(pal_http_parse_url("https://example.com", &tls, host, sizeof(host), &port, path, sizeof(path)), NEMA_OK);
    CHECK(tls && port == 443 && !strcmp(host, "example.com") && !strcmp(path, "/"));
    CHECK_EQ(pal_http_parse_url("http://h:8080/p/q?r=1#frag", &tls, host, sizeof(host), &port, path, sizeof(path)), NEMA_OK);
    CHECK(!tls && port == 8080 && !strcmp(host, "h") && !strcmp(path, "/p/q?r=1"));
    CHECK_EQ(pal_http_parse_url("http://h?q", &tls, host, sizeof(host), &port, path, sizeof(path)), NEMA_OK);
    CHECK(!strcmp(path, "/?q"));
    CHECK_EQ(pal_http_parse_url("ftp://h/", &tls, host, sizeof(host), &port, path, sizeof(path)), NEMA_ERR_INVAL);
    CHECK_EQ(pal_http_parse_url("http://:80/", &tls, host, sizeof(host), &port, path, sizeof(path)), NEMA_ERR_INVAL);
    CHECK_EQ(pal_http_parse_url("http://h:99999/", &tls, host, sizeof(host), &port, path, sizeof(path)), NEMA_ERR_INVAL);
}

TEST(tls_to_a_plain_server_fails_cleanly) {
    if (!strcmp(nema_net_tls_backend(), "none")) return; /* this build has no TLS */
    server(1, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", NULL);
    pal_http_req_t q = {.url = "https://127.0.0.1:39530/"};
    pal_http_resp_t r;
    nema_err_t e = get(&q, &r);
    CHECK(e != NEMA_OK);
    wait_server();
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_net_init();
    CHECK(nema_net_listen_bl(PORT, 4, &SV.lst) == NEMA_OK);
    RUN(content_length_body_and_headers);
    RUN(chunked_body_is_joined);
    RUN(a_redirect_is_followed_to_the_final_url);
    RUN(a_redirect_is_handed_back_when_not_followed_or_too_many);
    RUN(post_sends_its_body_and_headers);
    RUN(head_reads_no_body);
    RUN(on_data_can_stop_the_transfer);
    RUN(urls_are_split_and_bad_ones_refused);
    RUN(tls_to_a_plain_server_fails_cleanly);
    return TEST_DONE();
}
