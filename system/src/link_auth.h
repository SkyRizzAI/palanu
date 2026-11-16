/* Palanu Link pairing and authentication (docs/PLP.md, "Pairing" and "Auth"). The state machine works on payloads and
 * returns the reply; it does no I/O, so the host tests drive it directly with a fake clock and fake randomness.
 * link.c sends the replies and sets the connection's trust from them. */
#ifndef SYSTEM_LINK_AUTH_H
#define SYSTEM_LINK_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal/link.h"

/* ctl message types (PLP): the device side answers with the type after the request. */
#define LA_TYPE_PAIR_REQUEST 5
#define LA_TYPE_PAIR_CHALLENGE 6
#define LA_TYPE_PAIR_PROOF 7
#define LA_TYPE_PAIR_OK 8
#define LA_TYPE_AUTH_HELLO 9
#define LA_TYPE_AUTH_CHALLENGE 10
#define LA_TYPE_AUTH_RESPONSE 11
#define LA_TYPE_AUTH_OK 12
#define LA_TYPE_ERR 0x7F

#define LA_WINDOW_MS 120000u
#define LA_TRIES 3
#define LA_FAILS_MAX 5          /* failed AUTH_RESPONSE on one transport before the back-off */
#define LA_BACKOFF_MS 30000u

/* Clock and randomness, injectable for tests. NULL restores the system's (nema port). */
typedef struct {
    uint32_t (*now_ms)(void);
    void (*random)(void *buf, size_t n);
} la_env_t;
void la_set_env(const la_env_t *env);
/* Creates the lock. Call once from the task that creates the first link, before any other use. */
void la_init(void);

/* Per-connection state: one pairing exchange and one auth exchange may be open at a time. */
typedef struct {
    uint8_t pair_state; /* 0 idle, 1 challenged (PAIR_CHALLENGE sent, waiting for PAIR_PROOF) */
    uint8_t transcript[96]; /* client pub, device pub, client nonce, device nonce */
    uint8_t shared[32];     /* X25519 shared secret of the exchange */
    char name[25];          /* the client's name, NUL-terminated */
    uint8_t auth_open;      /* AUTH_CHALLENGE sent, waiting for AUTH_RESPONSE */
    uint8_t auth_id[8];
    uint8_t auth_nonce[16];
    uint8_t authed;         /* this connection authenticated as client_id */
    uint8_t client_id[8];
} la_sess_t;

/* What to send back. */
typedef struct {
    bool send;
    uint8_t type;
    size_t len;
    uint8_t payload[80];
    bool trust; /* AUTH_OK: the connection is authenticated now */
} la_reply_t;

void la_sess_reset(la_sess_t *s); /* forgets the exchanges and the authenticated client (wipes the secrets) */

/* Handles one ctl frame. tr is the connection's transport (rate limits are kept per transport). Non-pairing and
 * non-auth types are not its business: the caller routes them. */
void la_handle(la_sess_t *s, pal_link_transport_t tr, uint8_t type, const uint8_t *p, size_t n, la_reply_t *out);

/* The pairing window (device-wide). la_pair_open is also what `link pair` runs. */
nema_err_t la_pair_open(char code[PAL_LINK_CODE_LEN + 1]);
void la_pair_close(void);
size_t la_pair_status(char *out, size_t cap);

/* Rate limit: is the transport in its back-off now, and clearing it (tests). */
bool la_backoff(pal_link_transport_t tr);
void la_backoff_reset(void);

#endif
