/* The net module of the palanu API (api/palanu/net.h). A socket is a nema object the app holds as a handle, so the
 * handle table, close, and Ctrl-C work on it as on a file. The glue checks the capability; this file checks the handles,
 * keeps the new socket in the app's table, and maps the results. */
#include <stdint.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"
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
