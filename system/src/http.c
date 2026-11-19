/* HTTP/1.1 client (pal/http.h). One connection per request; a small buffered reader turns the socket into header lines and
 * body pieces. Buffers come from the heap (nema_alloc), so the caller's stack stays small. */
#include "pal/http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "nema/net.h"

#define RD_CAP 1024
#define LINE_MAX_HTTP 512

typedef struct {
    nema_obj_t *s;
    int32_t timeout;
    size_t pos, len;
    char buf[RD_CAP];
    char line[LINE_MAX_HTTP];
} rd_t;

/* ---- URL ---- */

nema_err_t pal_http_parse_url(const char *url, bool *tls, char *host, size_t host_cap, uint16_t *port, char *path, size_t path_cap) {
    if (!url) return NEMA_ERR_INVAL;
    const char *u;
    if (!strncmp(url, "http://", 7)) *tls = false, u = url + 7, *port = 80;
    else if (!strncmp(url, "https://", 8)) *tls = true, u = url + 8, *port = 443;
    else return NEMA_ERR_INVAL;
    size_t hl = strcspn(u, "/?#");
    const char *rest = u + hl;
    const char *colon = memchr(u, ':', hl);
    size_t name = colon ? (size_t)(colon - u) : hl;
    if (name == 0 || name >= host_cap) return NEMA_ERR_INVAL;
    memcpy(host, u, name);
    host[name] = 0;
    if (colon) {
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535) return NEMA_ERR_INVAL;
        *port = (uint16_t)p;
    }
    if (*rest == '#' || !*rest) rest = "/";
    size_t pl = strcspn(rest, "#");
    if (*rest == '?') { /* "host?q": the path is "/?q" */
        if (pl + 2 > path_cap) return NEMA_ERR_INVAL;
        path[0] = '/';
        memcpy(path + 1, rest, pl);
        path[pl + 1] = 0;
        return NEMA_OK;
    }
    if (pl + 1 > path_cap) return NEMA_ERR_INVAL;
    memcpy(path, rest, pl);
    path[pl] = 0;
    return NEMA_OK;
}

/* ---- reading ---- */

static int fill(rd_t *r) {
    r->pos = 0;
    int n = nema_obj_read(r->s, r->buf, RD_CAP, r->timeout);
    if (n == NEMA_ERR_PIPE) n = 0; /* the peer closed: end of stream */
    r->len = n > 0 ? (size_t)n : 0;
    return n;
}

/* The next line without CRLF into r->line. 1: a line, 0: end of stream before any byte, < 0: an error. */
static int read_line(rd_t *r) {
    size_t n = 0;
    bool any = false;
    for (;;) {
        if (r->pos >= r->len) {
            int got = fill(r);
            if (got < 0) return got;
            if (got == 0) {
                r->line[n] = 0;
                return any ? 1 : 0;
            }
        }
        char c = r->buf[r->pos++];
        any = true;
        if (c == '\n') break;
        if (c != '\r' && n + 1 < sizeof(r->line)) r->line[n++] = c;
    }
    r->line[n] = 0;
    return 1;
}

/* Hands up to `want` body bytes (all until the end of stream when want < 0) to on_data. */
static nema_err_t body_bytes(rd_t *r, int64_t want, const pal_http_req_t *q, pal_http_resp_t *rs) {
    while (want != 0) {
        if (r->pos >= r->len) {
            int got = fill(r);
            if (got < 0) return (nema_err_t)got;
            if (got == 0) return want < 0 ? NEMA_OK : NEMA_ERR_PROTO; /* cut short */
        }
        size_t n = r->len - r->pos;
        if (want > 0 && (int64_t)n > want) n = (size_t)want;
        if (q->on_data && q->on_data(q->ctx, r->buf + r->pos, n) < 0) return NEMA_ERR_CANCELED;
        r->pos += n;
        rs->received += n;
        if (want > 0) want -= (int64_t)n;
    }
    return NEMA_OK;
}

static nema_err_t chunked(rd_t *r, const pal_http_req_t *q, pal_http_resp_t *rs) {
    for (;;) {
        int l = read_line(r);
        if (l <= 0) return l < 0 ? (nema_err_t)l : NEMA_ERR_PROTO;
        char *end;
        long long size = strtoll(r->line, &end, 16);
        if (end == r->line || size < 0) return NEMA_ERR_PROTO;
        if (size == 0) { /* the trailer, up to the blank line */
            while ((l = read_line(r)) > 0 && r->line[0]) {
            }
            return NEMA_OK;
        }
        nema_err_t e = body_bytes(r, size, q, rs);
        if (e != NEMA_OK) return e;
        if (read_line(r) < 0) return NEMA_ERR_PROTO; /* the CRLF after the chunk */
    }
}

static bool header_is(const char *line, const char *name, const char **value) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) /* header names are ASCII and case-insensitive */
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) return false;
    if (line[n] != ':') return false;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return true;
}

/* ---- one exchange ---- */

static nema_err_t send_all(nema_obj_t *s, const void *p, size_t n, int32_t timeout) {
    int w = nema_obj_write(s, p, n, timeout);
    return w == (int)n ? NEMA_OK : w < 0 ? (nema_err_t)w : NEMA_ERR_TIMEOUT;
}

/* One request on one connection. *loc gets a redirect target when the answer is a 3xx with Location. */
static nema_err_t exchange(const pal_http_req_t *q, const char *url, const char *method, bool with_body, pal_http_resp_t *rs,
                           rd_t *r, char *loc, size_t loc_cap) {
    bool tls;
    char host[PAL_HTTP_HOST_MAX], path[PAL_HTTP_URL_MAX];
    uint16_t port;
    nema_err_t e = pal_http_parse_url(url, &tls, host, sizeof(host), &port, path, sizeof(path));
    if (e != NEMA_OK) return e;
    nema_obj_t *s = NULL;
    e = nema_net_connect(host, port, r->timeout, &s);
    if (e != NEMA_OK) return e;
    if (tls) e = nema_net_tls(s, host, !q->insecure, r->timeout);
    char *head = e == NEMA_OK ? nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, 1024) : NULL;
    if (e == NEMA_OK && !head) e = NEMA_ERR_NOMEM;
    if (e == NEMA_OK) {
        char hosthdr[PAL_HTTP_HOST_MAX + 8];
        if (port == (tls ? 443 : 80)) nema_strlcpy(hosthdr, host, sizeof(hosthdr));
        else snprintf(hosthdr, sizeof(hosthdr), "%s:%u", host, port);
        int n = snprintf(head, 1024, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PalanuOS\r\nAccept: */*\r\nConnection: close\r\n",
                         method, path, hosthdr);
        for (size_t i = 0; q->headers && q->headers[i] && n < 1000; i++) n += snprintf(head + n, 1024 - (size_t)n, "%s\r\n", q->headers[i]);
        if (with_body) n += snprintf(head + n, 1024 - (size_t)n, "Content-Length: %u\r\n", (unsigned)q->body_len);
        n += snprintf(head + n, 1024 - (size_t)n, "\r\n");
        e = n < 1024 ? send_all(s, head, (size_t)n, r->timeout) : NEMA_ERR_RANGE;
        if (e == NEMA_OK && with_body && q->body_len) e = send_all(s, q->body, q->body_len, r->timeout);
    }
    nema_free(head);
    r->s = s, r->pos = r->len = 0;
    if (e == NEMA_OK) { /* the status line */
        int l = read_line(r);
        if (l <= 0) e = l < 0 ? (nema_err_t)l : NEMA_ERR_PROTO;
        else if (strncmp(r->line, "HTTP/1.", 7) || strlen(r->line) < 12) e = NEMA_ERR_PROTO;
        else {
            rs->status = atoi(r->line + 9);
            if (q->on_header) q->on_header(q->ctx, r->line);
        }
    }
    bool is_chunked = false;
    rs->length = -1;
    rs->type[0] = 0;
    loc[0] = 0;
    while (e == NEMA_OK) { /* headers up to the blank line */
        int l = read_line(r);
        if (l <= 0) {
            e = l < 0 ? (nema_err_t)l : NEMA_ERR_PROTO;
            break;
        }
        if (!r->line[0]) break;
        if (q->on_header) q->on_header(q->ctx, r->line);
        const char *v;
        if (header_is(r->line, "Content-Length", &v)) rs->length = strtoll(v, NULL, 10);
        else if (header_is(r->line, "Transfer-Encoding", &v)) is_chunked = strstr(v, "chunked") != NULL;
        else if (header_is(r->line, "Location", &v)) nema_strlcpy(loc, v, loc_cap);
        else if (header_is(r->line, "Content-Type", &v)) nema_strlcpy(rs->type, v, sizeof(rs->type));
    }
    bool redirect = rs->status >= 300 && rs->status < 400 && loc[0];
    if (e == NEMA_OK && !redirect && strcmp(method, "HEAD") && rs->status != 204 && rs->status != 304) {
        if (is_chunked) e = chunked(r, q, rs);
        else e = body_bytes(r, rs->length, q, rs);
    }
    if (!redirect) loc[0] = 0;
    nema_obj_unref(s);
    return e;
}

/* A Location relative to the URL that answered: "/path" keeps the scheme, host and port; "path" replaces the last segment. */
static void resolve(const char *base, const char *loc, char *out, size_t cap) {
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) {
        nema_strlcpy(out, loc, cap);
        return;
    }
    const char *after = strstr(base, "://");
    after = after ? after + 3 : base;
    size_t origin = (size_t)(after - base) + strcspn(after, "/?#");
    if (loc[0] == '/') {
        snprintf(out, cap, "%.*s%s", (int)origin, base, loc);
        return;
    }
    const char *slash = strrchr(base + origin, '/');
    size_t keep = slash ? (size_t)(slash - base) + 1 : origin;
    snprintf(out, cap, "%.*s%s%s", (int)keep, base, slash ? "" : "/", loc);
}

nema_err_t pal_http_request(const pal_http_req_t *q, pal_http_resp_t *rs) {
    if (!q || !q->url || !rs) return NEMA_ERR_INVAL;
    memset(rs, 0, sizeof(*rs));
    rd_t *r = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*r));
    char *loc = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, PAL_HTTP_URL_MAX);
    if (!r || !loc) {
        nema_free(r), nema_free(loc);
        return NEMA_ERR_NOMEM;
    }
    r->timeout = q->timeout_ms > 0 ? q->timeout_ms : 15000;
    const char *method = q->method ? q->method : q->body ? "POST" : "GET";
    bool with_body = q->body != NULL || !strcmp(method, "POST") || !strcmp(method, "PUT") || !strcmp(method, "PATCH");
    nema_strlcpy(rs->url, q->url, sizeof(rs->url));
    nema_err_t e;
    for (int hop = 0;; hop++) {
        e = exchange(q, rs->url, method, with_body, rs, r, loc, PAL_HTTP_URL_MAX);
        if (e != NEMA_OK || !loc[0]) break;
        if (hop >= q->redirects) {
            if (q->redirects > 0) e = NEMA_ERR_RANGE;
            break;
        }
        char next[PAL_HTTP_URL_MAX];
        resolve(rs->url, loc, next, sizeof(next));
        nema_strlcpy(rs->url, next, sizeof(rs->url));
        if (rs->status == 303 || ((rs->status == 301 || rs->status == 302) && strcmp(method, "GET") && strcmp(method, "HEAD"))) {
            method = "GET"; /* what browsers do: the body is not sent again */
            with_body = false;
        }
        rs->received = 0;
    }
    nema_free(r);
    nema_free(loc);
    return e;
}
