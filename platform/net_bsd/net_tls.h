/* The TLS backend behind nema_net_tls (net_tls.c): non-blocking, on a socket's file descriptor. Private to platform/net_bsd. */
#ifndef PAL_NET_TLS_H
#define PAL_NET_TLS_H

#include <stdbool.h>
#include <stddef.h>

#include "nema/types.h"

typedef struct net_tls net_tls_t;

/* A client session for host (the name sent and checked against the certificate) on fd. verify false accepts any certificate. */
nema_err_t net_tls_open(int fd, const char *host, bool verify, net_tls_t **out);
/* NEMA_OK when done; NEMA_ERR_AGAIN with *want_read or *want_write set; NEMA_ERR_AUTH when the certificate is not trusted. */
int net_tls_handshake(net_tls_t *t, bool *want_read, bool *want_write);
/* Like a socket's read and write: >0 bytes, 0 end of stream (read), NEMA_ERR_AGAIN with the readiness it needs, <0 errors. */
int net_tls_read(net_tls_t *t, void *buf, size_t n, bool *want_read, bool *want_write);
int net_tls_write(net_tls_t *t, const void *buf, size_t n, bool *want_read, bool *want_write);
size_t net_tls_pending(net_tls_t *t); /* decrypted bytes waiting: a read will not block */
void net_tls_close(net_tls_t *t);     /* sends close_notify when it can; NULL is fine */
const char *net_tls_backend(void);    /* "mbedtls", "openssl" or "none" */

#endif
