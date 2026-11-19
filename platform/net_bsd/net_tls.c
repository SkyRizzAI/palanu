/* TLS over a socket of net_bsd.c (nema_net_tls): the backend that encrypts the bytes on the socket's file descriptor.
 *
 *   board  mbedtls (ESP-IDF's, 4.x) with the IDF certificate bundle; the chip's crypto blocks do the work. Its big buffers are
 *          allocated through the IDF's malloc, which puts them in PSRAM (CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC, SPIRAM_USE_MALLOC).
 *   host   OpenSSL when the build found it (PAL_HAVE_OPENSSL), with the system's trusted roots.
 *   other  none: nema_net_tls says NEMA_ERR_UNSUPPORTED (the browser simulator).
 *
 * Every call is non-blocking: NEMA_ERR_AGAIN with want_read / want_write saying which readiness to wait for. net_bsd.c does the
 * waiting with the pump, as for any socket. */
#include "net_tls.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include "nema/nema.h"

#if defined(ESP_PLATFORM)
#include "esp_crt_bundle.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"

struct net_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int fd;
};

static int bio_send(void *ctx, const unsigned char *b, size_t n) {
    ssize_t r = send(*(int *)ctx, b, n, MSG_DONTWAIT);
    if (r >= 0) return (int)r;
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n) {
    ssize_t r = recv(*(int *)ctx, b, n, MSG_DONTWAIT);
    if (r > 0) return (int)r;
    if (r == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

static int map(net_tls_t *t, int r, bool *want_read, bool *want_write) {
    if (r == MBEDTLS_ERR_SSL_WANT_READ) return *want_read = true, NEMA_ERR_AGAIN;
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE) return *want_write = true, NEMA_ERR_AGAIN;
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || (t && mbedtls_ssl_get_verify_result(&t->ssl) != 0)) return NEMA_ERR_AUTH;
    if (r == MBEDTLS_ERR_NET_SEND_FAILED || r == MBEDTLS_ERR_NET_RECV_FAILED) return NEMA_ERR_RESET;
    return NEMA_ERR_PROTO;
}

nema_err_t net_tls_open(int fd, const char *host, bool verify, net_tls_t **out) {
    static bool psa_ready;
    if (!psa_ready) {
        if (psa_crypto_init() != PSA_SUCCESS) return NEMA_ERR_NOMEM;
        psa_ready = true;
    }
    net_tls_t *t = nema_calloc(NEMA_MEM_EXTERNAL, NEMA_OWNER_KERNEL, sizeof(*t));
    if (!t) return NEMA_ERR_NOMEM;
    t->fd = fd;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    int r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0) {
        mbedtls_ssl_conf_authmode(&t->conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
        if (verify) r = esp_crt_bundle_attach(&t->conf) == ESP_OK ? 0 : -1;
    }
    if (r == 0) r = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (r == 0) r = mbedtls_ssl_set_hostname(&t->ssl, host);
    if (r != 0) {
        net_tls_close(t);
        return NEMA_ERR_NOMEM;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->fd, bio_send, bio_recv, NULL);
    *out = t;
    return NEMA_OK;
}

int net_tls_handshake(net_tls_t *t, bool *want_read, bool *want_write) {
    int r = mbedtls_ssl_handshake(&t->ssl);
    return r == 0 ? NEMA_OK : map(t, r, want_read, want_write);
}

int net_tls_read(net_tls_t *t, void *buf, size_t n, bool *want_read, bool *want_write) {
    int r = mbedtls_ssl_read(&t->ssl, buf, n);
    return r >= 0 ? r : map(t, r, want_read, want_write);
}

int net_tls_write(net_tls_t *t, const void *buf, size_t n, bool *want_read, bool *want_write) {
    int r = mbedtls_ssl_write(&t->ssl, buf, n);
    if (r > 0) return r;
    if (r == 0) return NEMA_ERR_AGAIN;
    return map(t, r, want_read, want_write);
}

size_t net_tls_pending(net_tls_t *t) { return mbedtls_ssl_get_bytes_avail(&t->ssl); }

void net_tls_close(net_tls_t *t) {
    if (!t) return;
    (void)mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    nema_free(t);
}

const char *net_tls_backend(void) { return "mbedtls"; }

#elif defined(PAL_HAVE_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

struct net_tls {
    SSL *ssl;
};

static SSL_CTX *ctx_for(bool verify) {
    static SSL_CTX *ctx[2];
    SSL_CTX **c = &ctx[verify ? 1 : 0];
    if (!*c) {
        *c = SSL_CTX_new(TLS_client_method());
        if (!*c) return NULL;
        SSL_CTX_set_min_proto_version(*c, TLS1_2_VERSION);
        SSL_CTX_set_verify(*c, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
        if (verify) SSL_CTX_set_default_verify_paths(*c);
    }
    return *c;
}

static int map(net_tls_t *t, int r, bool *want_read, bool *want_write) {
    int e = SSL_get_error(t->ssl, r);
    if (e == SSL_ERROR_WANT_READ) return *want_read = true, NEMA_ERR_AGAIN;
    if (e == SSL_ERROR_WANT_WRITE) return *want_write = true, NEMA_ERR_AGAIN;
    if (e == SSL_ERROR_ZERO_RETURN) return 0;
    if (SSL_get_verify_result(t->ssl) != X509_V_OK) return NEMA_ERR_AUTH;
    if (e == SSL_ERROR_SYSCALL) return errno == 0 ? 0 : NEMA_ERR_RESET;
    ERR_clear_error();
    return NEMA_ERR_PROTO;
}

nema_err_t net_tls_open(int fd, const char *host, bool verify, net_tls_t **out) {
    SSL_CTX *c = ctx_for(verify);
    if (!c) return NEMA_ERR_NOMEM;
    net_tls_t *t = nema_calloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*t));
    if (!t) return NEMA_ERR_NOMEM;
    t->ssl = SSL_new(c);
    if (!t->ssl) {
        nema_free(t);
        return NEMA_ERR_NOMEM;
    }
    SSL_set_fd(t->ssl, fd);
    SSL_set_tlsext_host_name(t->ssl, host);
    if (verify) X509_VERIFY_PARAM_set1_host(SSL_get0_param(t->ssl), host, 0); /* the certificate must name host */
    *out = t;
    return NEMA_OK;
}

int net_tls_handshake(net_tls_t *t, bool *want_read, bool *want_write) {
    int r = SSL_connect(t->ssl);
    return r == 1 ? NEMA_OK : map(t, r, want_read, want_write);
}

int net_tls_read(net_tls_t *t, void *buf, size_t n, bool *want_read, bool *want_write) {
    int r = SSL_read(t->ssl, buf, (int)n);
    return r > 0 ? r : map(t, r, want_read, want_write);
}

int net_tls_write(net_tls_t *t, const void *buf, size_t n, bool *want_read, bool *want_write) {
    int r = SSL_write(t->ssl, buf, (int)n);
    return r > 0 ? r : map(t, r, want_read, want_write);
}

size_t net_tls_pending(net_tls_t *t) { return (size_t)SSL_pending(t->ssl); }

void net_tls_close(net_tls_t *t) {
    if (!t) return;
    SSL_shutdown(t->ssl);
    SSL_free(t->ssl);
    nema_free(t);
}

const char *net_tls_backend(void) { return "openssl"; }

#else

nema_err_t net_tls_open(int fd, const char *host, bool verify, net_tls_t **out) {
    (void)fd, (void)host, (void)verify, (void)out;
    return NEMA_ERR_UNSUPPORTED;
}
int net_tls_handshake(net_tls_t *t, bool *want_read, bool *want_write) { return (void)t, (void)want_read, (void)want_write, NEMA_ERR_UNSUPPORTED; }
int net_tls_read(net_tls_t *t, void *buf, size_t n, bool *want_read, bool *want_write) {
    return (void)t, (void)buf, (void)n, (void)want_read, (void)want_write, NEMA_ERR_UNSUPPORTED;
}
int net_tls_write(net_tls_t *t, const void *buf, size_t n, bool *want_read, bool *want_write) {
    return (void)t, (void)buf, (void)n, (void)want_read, (void)want_write, NEMA_ERR_UNSUPPORTED;
}
size_t net_tls_pending(net_tls_t *t) { return (void)t, 0; }
void net_tls_close(net_tls_t *t) { (void)t; }
const char *net_tls_backend(void) { return "none"; }

#endif
