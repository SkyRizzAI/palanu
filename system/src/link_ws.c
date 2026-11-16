/* Palanu Link over WebSocket (RFC 6455): port 7001 on the board once Wi-Fi has an address, and `palanu-sim --ws PORT`.
 * A browser cannot open a raw TCP socket, so Web Forge (forge/web) reaches the device on Wi-Fi or LAN through this. The
 * WebSocket only carries the PLP byte stream: each message the client sends is fed to the link as it arrives, and each
 * transmit of the link goes out as one binary message. Frames may split PLP frames anywhere; the link's decoder joins them.
 *
 * One connection at a time, untrusted until AUTH (transport tag 4, docs/PLP.md "Trust"), closed after 60 s without AUTH,
 * as the TCP transport. No TLS: the page connects with ws://, so Web Forge is opened from a file or http://localhost
 * (an https page may not open ws://). The Origin header is not checked: the link's AUTH is what admits a client. */
#include <stdio.h>
#include <string.h>

#include "cryptoimpl.h"
#include "link_int.h"
#include "nema/nema.h"
#include "nema/port.h"

#define UNTRUSTED_IDLE_US 60000000ull
#define REQ_MAX 1024 /* the HTTP upgrade request */

static volatile uint32_t s_ws_gen;
static volatile bool s_ws_on;

static bool ws_alive(uint32_t gen8) { return s_ws_on && (s_ws_gen & 0xffu) == gen8; }

static int put_all(nema_obj_t *c, const void *d, size_t n) { return nema_obj_write(c, d, n, 5000) == (int)n ? 0 : -1; }

/* One unmasked binary message (server to client frames are never masked). PLP transmits are at most a frame (1031 B). */
static int ws_send(nema_obj_t *c, uint8_t opcode, const uint8_t *d, size_t n) {
    uint8_t h[10];
    size_t hl = 2;
    h[0] = (uint8_t)(0x80 | opcode);
    if (n < 126) h[1] = (uint8_t)n;
    else if (n <= 0xffff) {
        h[1] = 126;
        h[2] = (uint8_t)(n >> 8);
        h[3] = (uint8_t)n;
        hl = 4;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
        hl = 10;
    }
    if (put_all(c, h, hl) < 0) return -1;
    return n ? put_all(c, d, n) : 0;
}

static int ws_tx(void *ctx, const uint8_t *d, size_t n) { return ws_send((nema_obj_t *)ctx, 0x2, d, n); }

/* ---- the upgrade handshake ---- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* The value of header name (case-insensitive) in the request, trimmed, into out; false when absent. */
static bool header(const char *req, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *p = req; *p; p++) {
        if (p != req && p[-1] != '\n') continue;
        size_t i = 0;
        while (i < nl && p[i] && lower(p[i]) == lower(name[i])) i++;
        if (i != nl || p[i] != ':') continue;
        const char *v = p + i + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t k = 0;
        while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < cap) {
            out[k] = v[k];
            k++;
        }
        while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\t')) k--;
        out[k] = 0;
        return true;
    }
    return false;
}

/* Reads the request and answers it. Returns true when the connection is now a WebSocket; extra_at and nextra give bytes read
 * past the request (the first frames), which the caller feeds before reading more. */
static bool handshake(nema_obj_t *c, uint32_t gen8, uint8_t *buf, size_t cap, size_t *extra_at, size_t *nextra) {
    size_t n = 0;
    char *end = NULL;
    uint64_t t0 = nema_port_time_us();
    while (!end && ws_alive(gen8) && nema_port_time_us() - t0 < 10000000ull) {
        if (n + 1 >= cap) return false;
        int r = nema_obj_read(c, buf + n, cap - 1 - n, 500);
        if (r == NEMA_ERR_TIMEOUT) continue;
        if (r <= 0) return false;
        n += (size_t)r;
        buf[n] = 0;
        end = strstr((char *)buf, "\r\n\r\n");
    }
    if (!end) return false;
    char key[64], up[32];
    if (!header((char *)buf, "Sec-WebSocket-Key", key, sizeof(key)) || !header((char *)buf, "Upgrade", up, sizeof(up)) ||
        lower(up[0]) != 'w') {
        static const char NO[] = "HTTP/1.1 426 Upgrade Required\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                                 "Palanu Link: connect with WebSocket (Web Forge).\n";
        (void)put_all(c, NO, sizeof(NO) - 1);
        return false;
    }
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    ci_hash_t h;
    uint8_t dig[20];
    if (ci_hash_init(&h, CI_HASH_SHA1) != 0) return false;
    ci_hash_update(&h, key, strlen(key));
    ci_hash_update(&h, GUID, sizeof(GUID) - 1);
    ci_hash_final(&h, dig);
    char acc[32];
    base64(dig, sizeof(dig), acc);
    char resp[160];
    int rl = snprintf(resp, sizeof(resp),
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n",
                      acc);
    if (put_all(c, resp, (size_t)rl) < 0) return false;
    *extra_at = (size_t)(end + 4 - (char *)buf);
    *nextra = n - *extra_at;
    return true;
}

/* ---- client frames ---- */

typedef struct {
    int st;                /* 0 header byte 0, 1 byte 1, 2 extended length, 3 mask, 4 payload */
    uint8_t op, b1;
    uint8_t ext[8];
    int need, got;         /* bytes of the extended length still wanted */
    uint8_t mask[4];
    uint64_t left, off;    /* payload bytes still to come, and how many were unmasked */
    uint8_t ctl[125];      /* a control frame's payload (ping, close) */
    size_t nctl;
    bool closing;
} ws_rx_t;

/* Feeds n bytes of the stream: data payloads go to the link, pings are answered, close ends the session (returns false). */
static bool ws_feed(ws_rx_t *s, nema_obj_t *c, pal_link_t *l, uint8_t *d, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t b = d[i];
        switch (s->st) {
        case 0:
            s->op = b & 0x0f;
            s->st = 1;
            i++;
            break;
        case 1:
            s->b1 = b;
            s->left = b & 0x7f;
            s->off = 0;
            s->nctl = 0;
            i++;
            if (s->left == 126 || s->left == 127) {
                s->need = s->left == 126 ? 2 : 8;
                s->got = 0;
                s->left = 0;
                s->st = 2;
            } else {
                s->got = 0;
                s->st = 3;
            }
            if (!(s->b1 & 0x80)) return false; /* a client frame must be masked (RFC 6455 5.1) */
            break;
        case 2:
            s->left = (s->left << 8) | b;
            i++;
            if (++s->got == s->need) {
                s->got = 0;
                s->st = 3;
            }
            break;
        case 3:
            s->mask[s->got++] = b;
            i++;
            if (s->got == 4) s->st = 4;
            if (s->st == 4 && s->left == 0) goto frame_done;
            break;
        case 4: {
            size_t take = n - i < s->left ? n - i : (size_t)s->left;
            for (size_t k = 0; k < take; k++) d[i + k] ^= s->mask[(s->off + k) & 3];
            if (s->op == 0x0 || s->op == 0x1 || s->op == 0x2) pal_link_feed(l, d + i, take); /* continuation, text, binary */
            else {
                for (size_t k = 0; k < take && s->nctl < sizeof(s->ctl); k++) s->ctl[s->nctl++] = d[i + k];
            }
            s->off += take;
            s->left -= take;
            i += take;
            if (s->left) break;
        frame_done:
            s->st = 0;
            if (s->op == 0x9) (void)ws_send(c, 0xA, s->ctl, s->nctl); /* ping: pong with the same payload */
            else if (s->op == 0x8) {
                (void)ws_send(c, 0x8, s->ctl, s->nctl >= 2 ? 2 : 0); /* close: echo the status code */
                return false;
            }
            break;
        }
        default: return false;
        }
    }
    return true;
}

static void ws_server(void *arg) {
    uintptr_t v = (uintptr_t)arg;
    uint16_t port = (uint16_t)(v & 0xffffu);
    uint32_t gen8 = (uint32_t)(v >> 16) & 0xffu;
    nema_obj_t *ln = NULL;
    while (ws_alive(gen8) && nema_net_listen(port, &ln) != NEMA_OK) nema_sleep_ms(2000);
    uint8_t *buf = (uint8_t *)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, REQ_MAX); /* off the task's stack */
    while (buf && ws_alive(gen8)) {
        nema_obj_t *c;
        if (nema_net_accept(ln, 1000, &c) != NEMA_OK) continue;
        size_t at = 0, extra = 0;
        if (!ws_alive(gen8) || !handshake(c, gen8, buf, REQ_MAX, &at, &extra)) {
            nema_obj_unref(c);
            continue;
        }
        pal_link_io_t io = {.tx = ws_tx, .ctx = c, .transport = PAL_LINK_WS};
        pal_link_t *l = pal_link_create(&io);
        if (l) {
            ws_rx_t rx;
            memset(&rx, 0, sizeof(rx));
            bool open = ws_feed(&rx, c, l, buf + at, extra);
            uint64_t t0 = nema_port_time_us();
            while (open && ws_alive(gen8) && !l->kill && (l->trusted || nema_port_time_us() - t0 < UNTRUSTED_IDLE_US)) {
                int n = nema_obj_read(c, buf, REQ_MAX, 500);
                if (n == NEMA_ERR_TIMEOUT) continue;
                if (n <= 0) break;
                open = ws_feed(&rx, c, l, buf, (size_t)n);
            }
            pal_link_destroy(l);
        }
        nema_obj_unref(c);
    }
    nema_free(buf);
    if (ln) nema_obj_unref(ln);
}

nema_err_t pal_link_serve_ws(uint16_t port) {
    if (s_ws_on) return NEMA_OK;
    s_ws_gen++;
    s_ws_on = true;
    uintptr_t arg = ((uintptr_t)(s_ws_gen & 0xffu) << 16) | port;
    nema_task_desc_t td = {.name = "link-ws", .fn = ws_server, .arg = (void *)arg, .stack = 6144,
                           .prio = 3, .core = -1, .role = NEMA_TASK_SERVICE, .owner = nema_owner_register("link")};
    nema_err_t r = nema_task_create(&td);
    if (r != NEMA_OK) s_ws_on = false;
    return r;
}

void pal_link_stop_ws(void) {
    s_ws_on = false;
    s_ws_gen++;
}
