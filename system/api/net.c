/* The net module of the palanu API (api/palanu/net.h). A socket is a nema object the app holds as a handle, so the
 * handle table, close, and Ctrl-C work on it as on a file. The glue checks the capability; this file checks the handles,
 * keeps the new socket in the app's table, and maps the results. */
#include <stdint.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"
#include "pal/http.h"
#include "pal/wifi.h"

/* The size of the pal_net_info record (api/palanu/net.h, NET_INFO_LEN). */
#define NET_INFO_LEN 12

/* A canceled or interrupted call ends the app: the runtime is told to stop at the return. */
static int32_t cut(api_ctx_t *c, nema_err_t r) {
    if (r == NEMA_ERR_CANCELED || r == NEMA_ERR_INTR) api_stop(c, "interrupted");
    return r;
}

/* Puts a new object in the app's handle table. The caller's reference is dropped: the table holds the object now. */
static int32_t adopt(api_ctx_t *c, nema_obj_t *o) {
    int fd = nema_handle_install(o);
    nema_obj_unref(o);
    if (fd >= 0) api_fd_add(c, fd);
    return fd;
}

int32_t psys_api_tcp_connect(api_ctx_t *c, const char *host, int32_t port, int32_t timeout_ms) {
    if (!host || !*host || port < 1 || port > 65535) return NEMA_ERR_INVAL;
    if (!api_fd_has_slot(c)) return NEMA_ERR_FULL;
    nema_obj_t *o = NULL;
    nema_err_t r = nema_net_connect(host, (uint16_t)port, timeout_ms, &o);
    if (r != NEMA_OK) return cut(c, r);
    return adopt(c, o);
}

int32_t psys_api_tcp_listen(api_ctx_t *c, int32_t port, int32_t backlog) {
    if (port < 1 || port > 65535 || backlog < 1 || backlog > 32) return NEMA_ERR_INVAL;
    if (!api_fd_has_slot(c)) return NEMA_ERR_FULL;
    nema_obj_t *o = NULL;
    nema_err_t r = nema_net_listen_bl((uint16_t)port, backlog, &o);
    if (r != NEMA_OK) return r;
    return adopt(c, o);
}

int32_t psys_api_tcp_accept(api_ctx_t *c, int32_t listener, int32_t timeout_ms) {
    if (!api_fd_owned(c, listener)) return NEMA_ERR_BADF;
    if (!api_fd_has_slot(c)) return NEMA_ERR_FULL;
    nema_obj_t *o = NULL;
    nema_err_t r = nema_net_accept(nema_handle_obj(listener), timeout_ms, &o);
    if (r != NEMA_OK) return cut(c, r);
    return adopt(c, o);
}

int32_t psys_api_dns_resolve(api_ctx_t *c, const char *host, void *addr_out, uint32_t len, int32_t timeout_ms) {
    if (!host || len < 4) return NEMA_ERR_INVAL;
    uint8_t ip[4];
    nema_err_t r = nema_net_resolve_addr(host, timeout_ms, ip);
    if (r != NEMA_OK) return cut(c, r);
    memcpy(addr_out, ip, 4);
    return NEMA_OK;
}

/* a.b.c.d to four bytes; false when the text is not one. */
static bool parse_ipv4(const char *s, uint8_t out[4]) {
    unsigned v = 0, parts = 0, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p - '0');
            if (v > 255 || ++digits > 3) return false;
            continue;
        }
        if ((*p != '.' && *p != 0) || digits == 0 || parts == 4) return false;
        out[parts++] = (uint8_t)v;
        v = 0;
        digits = 0;
        if (*p == 0) break;
    }
    return parts == 4;
}

int32_t psys_api_net_info(api_ctx_t *c, void *out, uint32_t len) {
    (void)c;
    if (len < NET_INFO_LEN) return NEMA_ERR_INVAL;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    uint8_t *o = (uint8_t *)out;
    memset(o, 0, NET_INFO_LEN);
    o[0] = st.connected ? 1 : 0;
    if (st.connected) {
        st.ip[sizeof(st.ip) - 1] = 0;
        parse_ipv4(st.ip, o + 4);
    }
    uint32_t rssi = st.connected ? (uint32_t)(int32_t)st.rssi : 0;
    o[8] = (uint8_t)rssi;
    o[9] = (uint8_t)(rssi >> 8);
    o[10] = (uint8_t)(rssi >> 16);
    o[11] = (uint8_t)(rssi >> 24);
    return NEMA_OK;
}

int32_t psys_api_set_nonblock(api_ctx_t *c, int32_t h, int32_t on) {
    if (!api_fd_owned(c, h)) return NEMA_ERR_BADF;
    nema_obj_t *o = nema_handle_obj(h);
    if (!o) return NEMA_ERR_BADF;
    nema_obj_set_nonblock(o, on != 0);
    return NEMA_OK;
}

int32_t psys_api_shutdown(api_ctx_t *c, int32_t h, int32_t how) {
    if (!api_fd_owned(c, h)) return NEMA_ERR_BADF;
    int v = (int)how;
    return nema_ioctl(h, NEMA_NETIOC_SHUTDOWN, &v);
}

/* ---- TLS and HTTP (API level 7) ---- */

int32_t psys_api_tls_start(api_ctx_t *c, int32_t h, const char *host, int32_t timeout_ms) {
    if (!api_fd_owned(c, h)) return NEMA_ERR_BADF;
    if (!host || !*host) return NEMA_ERR_INVAL;
    return cut(c, nema_net_tls(nema_handle_obj(h), host, true, timeout_ms));
}

typedef struct {
    uint8_t *out;
    uint32_t cap, used;
} fetch_sink_t;

static int fetch_data(void *ctx, const void *buf, size_t n) {
    fetch_sink_t *f = (fetch_sink_t *)ctx;
    size_t room = f->cap - f->used;
    if (n > room) n = room; /* the rest is counted (resp.received) and dropped */
    memcpy(f->out + f->used, buf, n);
    f->used += (uint32_t)n;
    return 0;
}

#define HTTP_HEADERS_MAX 8

int32_t psys_api_http_fetch(api_ctx_t *c, const char *method, const char *url, const char *headers, const void *body, uint32_t body_len,
                            void *out, uint32_t cap, int32_t timeout_ms) {
    if (!url || !out || cap < 8) return NEMA_ERR_INVAL;
    static const char *const METHODS[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "PATCH"};
    const char *m = NULL;
    for (size_t i = 0; method && i < sizeof(METHODS) / sizeof(METHODS[0]); i++)
        if (!strcmp(method, METHODS[i])) m = METHODS[i];
    if (!m) return NEMA_ERR_INVAL;
    /* the header lines: copied, split on newlines (no CR or LF reaches the request inside a value) */
    char *hb = NULL;
    const char *hv[HTTP_HEADERS_MAX + 1];
    int nh = 0;
    if (headers && *headers) {
        size_t hl = strlen(headers);
        if (hl > 1024) return NEMA_ERR_RANGE;
        hb = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, hl + 1);
        if (!hb) return NEMA_ERR_NOMEM;
        memcpy(hb, headers, hl + 1);
        for (char *p = hb; *p && nh < HTTP_HEADERS_MAX;) {
            char *e = p + strcspn(p, "\r\n");
            bool more = *e != 0;
            *e = 0;
            if (*p) hv[nh++] = p;
            p = more ? e + 1 : e;
            while (*p == '\r' || *p == '\n') p++;
        }
    }
    hv[nh] = NULL;
    fetch_sink_t sink = {(uint8_t *)out + 8, cap - 8, 0};
    pal_http_req_t q = {.method = m, .url = url, .headers = nh ? hv : NULL, .body = body_len ? body : NULL, .body_len = body_len,
                        .redirects = 5, .timeout_ms = timeout_ms, .on_data = fetch_data, .ctx = &sink};
    pal_http_resp_t r;
    nema_err_t e = pal_http_request(&q, &r);
    nema_free(hb);
    if (e != NEMA_OK) return cut(c, e);
    int32_t st = r.status, total = r.received > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)r.received;
    uint8_t *o = (uint8_t *)out;
    for (int i = 0; i < 4; i++) o[i] = (uint8_t)(st >> (8 * i)), o[4 + i] = (uint8_t)(total >> (8 * i));
    return (int32_t)(8 + sink.used);
}
