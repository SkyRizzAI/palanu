/* The per-process tables of the crypto API (system/api/crypto*.c, keys.c): 8 digests, 8 ciphers and 8 loaded keys per
 * process, in one block charged to the process. The sweep at exit frees it, so nothing is closed by hand. Handles are
 * indexes into these tables. */
#ifndef SYSTEM_CRYPTO_TAB_H
#define SYSTEM_CRYPTO_TAB_H

#include <stdint.h>

#include "api_ctx.h"
#include "crypto_mac.h"
#include "cryptoimpl.h"

#define CT_HASH_MAX 8
#define CT_CIPHER_MAX 8
#define CT_KEY_MAX 8

typedef struct {
    uint8_t used;
    crypto_mac_t mac; /* a hash or an HMAC */
} ct_hash_t;

enum { CT_AEAD = 1, CT_CTR = 2 };
typedef struct {
    uint8_t used;
    uint8_t kind; /* CT_AEAD or CT_CTR */
    uint8_t alg;  /* CI_AEAD_* for an AEAD */
    uint8_t key[32]; /* ChaCha20-Poly1305 keeps its key here; the AES forms keep it inside ci_cipher_t */
    ci_cipher_t c;
} ct_cipher_t;

enum { CT_KEY_ED25519 = 1, CT_KEY_ECDSA_P256 = 2 };
typedef struct {
    uint8_t used;
    uint8_t type;      /* CT_KEY_* */
    uint8_t secret[64]; /* ed25519: seed || public key (Monocypher's 64-byte form); ecdsa: scalar in the first 32 */
    uint8_t pub[65];    /* ed25519: 32 bytes; ecdsa: X || Y */
} ct_key_t;

typedef struct {
    ct_hash_t hash[CT_HASH_MAX];
    ct_cipher_t cipher[CT_CIPHER_MAX];
    ct_key_t key[CT_KEY_MAX];
} crypto_tab_t;

/* The table of the calling app, made on first use and charged to its process. c NULL is a built-in command: its own
 * process's table (also charged to it and swept at exit); a call from no process uses one kernel table. NULL when out of
 * memory. */
crypto_tab_t *crypto_tab(api_ctx_t *c);

#endif
