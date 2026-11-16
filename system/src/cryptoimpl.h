/* The accelerated primitives behind the crypto API. Two implementations sit behind this interface, chosen at
 * build time by ESP_PLATFORM:
 *   cryptoimpl_mbedtls.c  ESP: PSA Crypto from TF-PSA-Crypto in ESP-IDF, which uses the chip's AES, SHA and ECC blocks.
 *   cryptoimpl_soft*.c    host: portable C, also the reference the tests check against. ECDSA and RSA are not in it:
 *                         they return NEMA_ERR_UNSUPPORTED there (the host tests skip them).
 * Everything else (HMAC, ChaCha20, Poly1305, X25519, Ed25519) is shared: crypto_mac.c and crypto_prim.c, with Monocypher.
 *
 * Opaque state: each implementation stores its own context inside the fixed-size buffers below. Its sizes are checked
 * by _Static_assert in that implementation. */
#ifndef SYSTEM_CRYPTOIMPL_H
#define SYSTEM_CRYPTOIMPL_H

#include <stddef.h>
#include <stdint.h>

/* Hash algorithms (the values the palanu API uses as PAL_HASH_*). */
#define CI_HASH_SHA1 1
#define CI_HASH_SHA256 2
#define CI_HASH_SHA384 3
#define CI_HASH_SHA512 4

/* Cipher algorithms for the AEAD table (PAL_AEAD_*): AES by key length, ChaCha20-Poly1305 is Monocypher's. */
#define CI_AEAD_AES128_GCM 1
#define CI_AEAD_AES256_GCM 2
#define CI_AEAD_CHACHA20_POLY1305 3

#define CI_HASH_STATE_MAX 384
#define CI_CIPHER_STATE_MAX 512

typedef struct {
    uint64_t raw[CI_HASH_STATE_MAX / 8];
} ci_hash_t;

typedef struct {
    uint64_t raw[CI_CIPHER_STATE_MAX / 8];
} ci_cipher_t;

/* Digests. ci_hash_init returns 0, or NEMA_ERR_UNSUPPORTED for an unknown algorithm. ci_hash_final writes
 * ci_hash_len(alg) bytes to out and wipes the state. */
size_t ci_hash_len(int alg);
size_t ci_hash_block(int alg);
int ci_hash_init(ci_hash_t *h, int alg);
void ci_hash_update(ci_hash_t *h, const void *d, size_t n);
void ci_hash_final(ci_hash_t *h, uint8_t *out);

/* AES-GCM with a 12-byte nonce and a 16-byte tag. Seal encrypts buf in place; open decrypts in place and returns
 * NEMA_ERR_AUTH when the tag does not match; the buffer is then left unchanged. */
int ci_gcm_init(ci_cipher_t *c, const uint8_t *key, size_t klen); /* klen 16 or 32 */
int ci_gcm_seal(ci_cipher_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf, size_t len,
                uint8_t tag[16]);
int ci_gcm_open(ci_cipher_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf, size_t len,
                const uint8_t tag[16]);

/* AES-CTR with a 16-byte initial counter block (big-endian increment over the whole block). */
int ci_ctr_init(ci_cipher_t *c, const uint8_t *key, size_t klen, const uint8_t iv[16]);
void ci_ctr_xor(ci_cipher_t *c, uint8_t *buf, size_t len);

/* ECDSA on NIST P-256. The public key is X || Y (64 bytes), the signature r || s (64 bytes), the hash SHA-256 (32). */
int ci_ecdsa_p256_verify(const uint8_t pub[64], const uint8_t *hash, size_t hlen, const uint8_t sig[64]);
int ci_ecdsa_p256_generate(uint8_t priv[32], uint8_t pub[64]);
int ci_ecdsa_p256_sign(const uint8_t priv[32], const uint8_t *hash, size_t hlen, uint8_t sig[64]);

/* RSA PKCS#1 v1.5 verification. n and e are big-endian integers without leading zeros. halg is a CI_HASH_*. */
int ci_rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg, const uint8_t *hash,
                        size_t hlen, const uint8_t *sig, size_t slen);

#endif
