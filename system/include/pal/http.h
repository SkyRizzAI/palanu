/* PalanuOS HTTP client: HTTP/1.1 over Nema sockets, HTTPS through nema_net_tls. One request per connection (Connection: close),
 * any method, extra headers, a body, redirects, chunked and Content-Length bodies. The body is handed over in pieces through
 * on_data, so a download of any size needs no big buffer. curl, the app API (api/palanu/net.h) and the app catalogue use it.
 * Blocking: call it from a process or a task that may wait, never from the desktop task. */
#ifndef PAL_HTTP_H
#define PAL_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"

#define PAL_HTTP_URL_MAX 256
#define PAL_HTTP_HOST_MAX 96

typedef struct {
    const char *method;          /* NULL: GET, or POST when there is a body */
    const char *url;             /* http://host[:port]/path or https://... */
    const char *const *headers;  /* extra "Name: value" lines, NULL-terminated; NULL for none */
    const void *body;
    size_t body_len;
    bool insecure;               /* https: accept any certificate (curl -k) */
    int redirects;               /* follow up to this many 3xx answers; 0 hands the 3xx back */
    int32_t timeout_ms;          /* each wait (connect, handshake, a read); <= 0: 15 s */
    void (*on_header)(void *ctx, const char *line); /* the status line and each header, without CRLF; NULL: not wanted */
    int (*on_data)(void *ctx, const void *buf, size_t n); /* body bytes; < 0 stops the transfer (NEMA_ERR_CANCELED) */
    void *ctx;
} pal_http_req_t;

typedef struct {
    int status;                  /* 200, 404...; 0 when no answer came */
    int64_t length;              /* Content-Length, or -1 when the server did not say */
    uint64_t received;           /* body bytes handed to on_data */
    char url[PAL_HTTP_URL_MAX];  /* the URL that answered (after redirects) */
    char type[64];               /* Content-Type, or "" */
} pal_http_resp_t;

/* Runs the request. NEMA_OK means an answer came (any status, see resp->status). Errors: NEMA_ERR_INVAL (a bad URL),
 * the connect and TLS errors (NEMA_ERR_AUTH: the certificate is not trusted, NEMA_ERR_UNSUPPORTED: no TLS in this build),
 * NEMA_ERR_PROTO (not HTTP), NEMA_ERR_TIMEOUT, NEMA_ERR_CANCELED (on_data stopped it), NEMA_ERR_RANGE (too many redirects). */
nema_err_t pal_http_request(const pal_http_req_t *req, pal_http_resp_t *resp);

/* Splits url into its parts. NEMA_ERR_INVAL when the scheme is not http or https or a part does not fit. */
nema_err_t pal_http_parse_url(const char *url, bool *tls, char *host, size_t host_cap, uint16_t *port, char *path, size_t path_cap);

#endif
