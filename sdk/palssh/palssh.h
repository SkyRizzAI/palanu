/* palssh: an SSH-2 client (RFC 4253 transport, RFC 4252 user authentication, RFC 4254 connection) written only
 * against the public PalanuOS app API, plus the algorithms OpenSSH servers offer first (see docs/TESTING-SSH.md).
 *
 *     #define PALSSH_IMPLEMENTATION   // in exactly one .c file of the app, before the include
 *     #define PALSSH_WITH_PAL         // the default backend: sockets, stdio, keystore and crypto through palanu.h
 *     #include "palssh/palssh.h"
 *
 * Tests on the host define PALSSH_IMPLEMENTATION and give their own backend (pssh_ops_t) on top of the system's
 * primitives; the library itself never calls a platform function. It uses no heap: every buffer is a member of
 * pssh_t, so the app declares one static pssh_t (it is about 80 KB, too big for the stack).
 *
 * Flow: pssh_init, set the config fields, pssh_handshake, pssh_authenticate, pssh_session_open, pssh_session_run.
 * Every call that waits takes a timeout and returns when a stop request (PSSH_EV_QUIT from ops->event) arrives. */
#ifndef PALSSH_H
#define PALSSH_H

#include <stddef.h>
#include <stdint.h>

#define PSSH_VERSION_LINE "SSH-2.0-PalanuOS_1.0"
#define PSSH_MAX_PAYLOAD 35000u /* RFC 4253 packet_length limit we accept and send */
#define PSSH_BUF 35104u         /* one packet on the wire at most: 4 + 35000 + 16 (tag) */
#define PSSH_KEXINIT_MAX 2048u  /* a server KEXINIT larger than this is refused */
#define PSSH_HBUF 6144u         /* exchange hash input: the strings of RFC 4253 section 8 */
#define PSSH_WINDOW 65536u      /* our initial channel window */
#define PSSH_CHAN_MAXPKT 16384u /* our maximum channel packet */
#define PSSH_NAME_MAX 64
#define PSSH_HOST_MAX 128
#define PSSH_SECRET_MAX 128

/* Results: 0 is success, the rest are negative. */
#define PSSH_OK 0
#define PSSH_E_AGAIN (-1)       /* no data yet (non-blocking transport) */
#define PSSH_E_EOF (-2)         /* the peer closed the connection */
#define PSSH_E_IO (-3)          /* the backend failed */
#define PSSH_E_PROTO (-4)       /* malformed or unexpected message, or a bad MAC */
#define PSSH_E_KEX (-5)         /* key exchange failed (signature, algorithms, low-order point) */
#define PSSH_E_AUTH (-6)        /* no authentication method succeeded */
#define PSSH_E_REFUSED (-7)     /* the host key callback refused the key */
#define PSSH_E_UNSUPPORTED (-8) /* the backend cannot do this algorithm (e.g. ECDSA on the simulator) */
#define PSSH_E_TIMEOUT (-9)
#define PSSH_E_ARG (-10)
#define PSSH_E_QUIT (-11)      /* the app was asked to stop */
#define PSSH_E_CHANNEL (-12)   /* the server refused the session or a request */
#define PSSH_E_NOKEY (-13)     /* the named keystore key does not exist */

/* Wait bits for ops->wait. */
#define PSSH_W_SOCK_IN 1u
#define PSSH_W_SOCK_OUT 2u
#define PSSH_W_STDIN 4u
#define PSSH_W_EVENT 8u

/* Events from ops->event. */
#define PSSH_EV_NONE 0
#define PSSH_EV_RESIZE 1
#define PSSH_EV_QUIT 2

/* Hash and HMAC algorithms: the numbers of PAL_HASH_* (api/palanu/crypto.h). */
#define PSSH_H_SHA1 1
#define PSSH_H_SHA256 2
#define PSSH_H_SHA384 3
#define PSSH_H_SHA512 4

/* Packet ciphers negotiated for a direction. */
#define PSSH_C_NONE 0
#define PSSH_C_GCM128 1
#define PSSH_C_GCM256 2
#define PSSH_C_CHACHA 3

/* Message numbers (RFC 4250 section 4.1 and the connection and auth protocols). */
#define SSH_MSG_DISCONNECT 1
#define SSH_MSG_IGNORE 2
#define SSH_MSG_UNIMPLEMENTED 3
#define SSH_MSG_DEBUG 4
#define SSH_MSG_SERVICE_REQUEST 5
#define SSH_MSG_SERVICE_ACCEPT 6
#define SSH_MSG_EXT_INFO 7
#define SSH_MSG_KEXINIT 20
#define SSH_MSG_NEWKEYS 21
#define SSH_MSG_KEX_ECDH_INIT 30
#define SSH_MSG_KEX_ECDH_REPLY 31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_USERAUTH_BANNER 53
#define SSH_MSG_USERAUTH_PK_OK 60
#define SSH_MSG_USERAUTH_INFO_REQUEST 60
#define SSH_MSG_USERAUTH_INFO_RESPONSE 61
#define SSH_MSG_GLOBAL_REQUEST 80
#define SSH_MSG_REQUEST_SUCCESS 81
#define SSH_MSG_REQUEST_FAILURE 82
#define SSH_MSG_CHANNEL_OPEN 90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA 94
#define SSH_MSG_CHANNEL_EXTENDED_DATA 95
#define SSH_MSG_CHANNEL_EOF 96
#define SSH_MSG_CHANNEL_CLOSE 97
#define SSH_MSG_CHANNEL_REQUEST 98
#define SSH_MSG_CHANNEL_SUCCESS 99
#define SSH_MSG_CHANNEL_FAILURE 100

/* A byte range. */
typedef struct {
    const uint8_t *p;
    size_t n;
} pssh_bytes_t;

/* The backend. The library calls these and nothing else from the platform. Calls return a count or 0 for success,
 * and a negative PSSH_E_* on failure. Buffers passed in are valid for the call only.
 *
 *   sock_read / sock_write: non-blocking; PSSH_E_AGAIN when no data or no room; sock_read returns 0 at end of stream.
 *   stdin_read: returns bytes, 0 at end of input; stdout_write(fd) writes to fd 1 or 2 and returns all of n.
 *   wait: waits up to timeout_ms (-1 forever) until one of the want bits is ready; got gets the ready bits. It must
 *         also return when an event is pending (PSSH_W_EVENT) so the stop request and resizes are seen.
 *   event: takes one pending event: PSSH_EV_RESIZE with cols and rows, PSSH_EV_QUIT, or PSSH_EV_NONE.
 *   size: the terminal size (cols, rows).
 *   random, hash, hmac: PSSH_H_* algorithms. x25519(out, scalar, NULL) is the base point.
 *   ed25519_verify, ecdsa_p256_verify, rsa_verify: 0 when the signature is good, PSSH_E_AUTH when it is not,
 *         PSSH_E_UNSUPPORTED when the backend cannot check that algorithm.
 *   chacha20: RFC 8439 style keystream with the 64-bit nonce of OpenSSH (8 bytes) and a 64-bit block counter.
 *   poly1305: the raw one-time authenticator.
 *   aead_open: starts an AES-GCM context with the key and returns a handle; aead_seal/aead_decrypt work in place with
 *         a 12-byte nonce, the additional data, and the 16-byte tag.
 *   key_load / key_public / key_sign: the keystore. key_load returns a handle (>= 0) or a negative error.
 *   secret: reads a line without echo after writing prompt; returns its length. */
typedef struct pssh_ops {
    void *ctx;
    int (*sock_read)(void *ctx, uint8_t *buf, size_t n);
    int (*sock_write)(void *ctx, const uint8_t *buf, size_t n);
    int (*stdin_read)(void *ctx, uint8_t *buf, size_t n);
    int (*stdout_write)(void *ctx, int fd, const uint8_t *buf, size_t n);
    int (*wait)(void *ctx, unsigned want, int timeout_ms, unsigned *got);
    int (*event)(void *ctx, int *kind, int *cols, int *rows);
    int (*size)(void *ctx, int *cols, int *rows);
    int64_t (*now_ms)(void *ctx);
    int (*random)(void *ctx, uint8_t *buf, size_t n);
    int (*hash)(void *ctx, int alg, const uint8_t *d, size_t n, uint8_t *out);
    int (*hmac)(void *ctx, int alg, const uint8_t *key, size_t klen, const uint8_t *d, size_t n, uint8_t *out);
    int (*x25519)(void *ctx, uint8_t out[32], const uint8_t scalar[32], const uint8_t *point);
    int (*ed25519_verify)(void *ctx, const uint8_t pk[32], const uint8_t *msg, size_t n, const uint8_t sig[64]);
    int (*ecdsa_p256_verify)(void *ctx, const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]);
    int (*rsa_verify)(void *ctx, const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg,
                      const uint8_t *hash, size_t hlen, const uint8_t *sig, size_t slen);
    int (*chacha20)(void *ctx, const uint8_t key[32], const uint8_t nonce[8], uint64_t counter, const uint8_t *in,
                    uint8_t *out, size_t n);
    int (*poly1305)(void *ctx, const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t tag[16]);
    int (*aead_open)(void *ctx, int alg, const uint8_t *key, size_t klen);
    int (*aead_seal)(void *ctx, int h, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                     size_t n, uint8_t tag[16]);
    int (*aead_decrypt)(void *ctx, int h, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                        size_t n, const uint8_t tag[16]);
    int (*key_load)(void *ctx, const char *name, size_t nlen);
    int (*key_public)(void *ctx, int h, uint8_t *out, size_t cap);
    int (*key_sign)(void *ctx, int h, const uint8_t *d, size_t n, uint8_t *sig, size_t cap);
    int (*secret)(void *ctx, const char *prompt, char *buf, size_t cap);
} pssh_ops_t;

/* One direction of the packet protection. */
typedef struct {
    int kind;                  /* PSSH_C_* */
    int h;                     /* aead handle for GCM */
    uint8_t key[64];           /* chacha: main key then header key; gcm: the AES key */
    uint8_t iv[12];            /* gcm: the nonce; its last 8 bytes count up per packet */
    uint32_t seq;              /* RFC 4253 sequence number (wraps at 2^32) */
} pssh_dir_t;

/* Host key check, called once the server's signature is verified. Returns 0 to accept, anything else refuses. */
typedef int (*pssh_hostkey_fn)(void *ctx, const char *host, uint16_t port, const char *alg, const uint8_t *blob,
                               size_t n);

typedef struct pssh {
    pssh_ops_t ops;
    /* config: the app sets these before the handshake */
    char user[PSSH_NAME_MAX];
    char host[PSSH_HOST_MAX];
    uint16_t port;
    char key_name[PSSH_NAME_MAX]; /* keystore key for publickey, "" for none */
    int want_pty;                 /* 1: request a terminal in the session */
    char term[32];
    pssh_hostkey_fn hostkey_check;
    void *hostkey_ctx;
    /* identification and key exchange */
    char v_s[256];
    size_t v_s_len;
    uint8_t i_c[PSSH_KEXINIT_MAX];
    size_t i_c_len;
    uint8_t i_s[PSSH_KEXINIT_MAX];
    size_t i_s_len;
    uint8_t sid[32];
    int have_sid;
    int kex_done;
    int strict;     /* kex-strict agreed: sequence numbers restart at NEWKEYS */
    int cipher_c2s; /* PSSH_C_* negotiated */
    int cipher_s2c;
    int hostkey_alg; /* 1 ed25519, 2 rsa-sha2-256, 3 rsa-sha2-512, 4 ecdsa-sha2-nistp256 */
    uint8_t eph[32];    /* our ephemeral curve25519 secret, wiped after the reply */
    uint8_t qc[32];     /* our ephemeral public key (Q_C) */
    uint8_t kshared[32]; /* K: the shared secret, from the reply until the server's NEWKEYS */
    uint8_t hsh[32];     /* H: the exchange hash of the same window */
    pssh_dir_t out_dir;
    pssh_dir_t in_dir;
    /* transport buffers: received bytes and bytes waiting to be sent */
    uint8_t in[PSSH_BUF];
    size_t in_len;
    size_t in_used; /* bytes of the last returned packet still in `in` */
    uint8_t out[PSSH_BUF];
    size_t out_len;
    size_t out_off;
    uint8_t hbuf[PSSH_HBUF];
    /* channel */
    int chan_open;
    int chan_closed;
    int chan_eof_in;
    int chan_eof_sent;
    int esc_quit;   /* `~.` was typed: disconnect and exit 255 */
    int resized;    /* a terminal resize is waiting to be sent */
    int chan_req_ok;
    int chan_req_fail;
    int have_exit;
    int exit_status;
    uint32_t chan_remote;
    uint32_t win_remote;
    uint32_t maxpkt_remote;
    uint32_t win_adjust_due; /* bytes consumed on our side not yet told to the server */
    int at_bol;              /* the last byte typed was a line start: `~.` is an escape here */
    int esc_pending;         /* a `~` was typed at line start; the next byte decides */
    int disconnected;        /* we sent DISCONNECT */
    int sent_close;
    char banner[256];
    int quit;
    int cols, rows;
} pssh_t;

/* ---- error text and helpers (palssh_util.inc) ---- */
const char *pssh_strerror(int err);
size_t pssh_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
int pssh_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap); /* bytes, or PSSH_E_PROTO */
/* "SHA256:" plus the base64 of the SHA-256 of the key blob, without padding (as ssh-keygen prints). */
int pssh_fingerprint(const pssh_ops_t *ops, const uint8_t *blob, size_t n, char *out, size_t cap);

/* ---- session setup ---- */
int pssh_init(pssh_t *s, const pssh_ops_t *ops);
int pssh_handshake(pssh_t *s, int timeout_ms);
int pssh_authenticate(pssh_t *s, int timeout_ms);
int pssh_session_open(pssh_t *s, const char *command, int cols, int rows);
/* Runs the session until the channel closes: returns the remote exit status, or a negative PSSH_E_*. */
int pssh_session_run(pssh_t *s);
void pssh_disconnect(pssh_t *s, uint32_t reason, const char *text);

/* ---- transport internals, exposed for the host tests (palssh_transport.inc) ---- */
/* Computes the exchange hash H over V_C, V_S, I_C, I_S, K_S, Q_C, Q_S (in that order) and the shared secret K. */
int pssh_exchange_hash(const pssh_ops_t *ops, const pssh_bytes_t parts[7], const uint8_t k[32], uint8_t out[32]);
/* RFC 4253 section 7.2: key letter 'A'..'F' with the first 32 bytes of K1, K2... (n bytes into out). */
int pssh_derive(const pssh_ops_t *ops, const uint8_t k[32], const uint8_t h[32], const uint8_t sid[32], char letter,
                uint8_t *out, size_t n);
/* Installs the keys of a finished exchange: c2s for the sending direction, s2c for the receiving one. */
int pssh_install_keys(pssh_t *s, int cipher_c2s, int cipher_s2c, const uint8_t k[32], const uint8_t h[32]);
/* Packets: pssh_send encodes one payload into the output buffer (and flushes when it can),
 * pssh_recv_try returns the next complete payload or PSSH_E_AGAIN. */
int pssh_send(pssh_t *s, const uint8_t *payload, size_t n);
int pssh_flush(pssh_t *s);
int pssh_recv_try(pssh_t *s, const uint8_t **payload, size_t *n);

/* ---- known_hosts (palssh_hosts.inc): the file is given as text ---- */
#define PSSH_HOST_NEW 0
#define PSSH_HOST_FOUND 1
#define PSSH_HOST_CHANGED 2
int pssh_hosts_lookup(const pssh_ops_t *ops, const char *text, size_t tlen, const char *host, uint16_t port,
                      const char *alg, const uint8_t *blob, size_t blen);
/* Writes "host keytype base64" (with [host]:port for non-22) followed by a newline. Returns its length or < 0. */
int pssh_hosts_line(const char *host, uint16_t port, const char *alg, const uint8_t *blob, size_t blen, char *out,
                    size_t cap);

#ifdef PALSSH_IMPLEMENTATION
#ifdef PALSSH_WITH_PAL
#include "../palanu.h"
#endif
/* One translation unit, split into parts next to this header (kept under the 800-line rule). */
#include "palssh_util.inc"
#include "palssh_transport.inc"
#include "palssh_auth.inc"
#include "palssh_chan.inc"
#include "palssh_hosts.inc"
#ifdef PALSSH_WITH_PAL
#include "palssh_pal.inc"
#endif
#endif

#endif /* PALSSH_H */
