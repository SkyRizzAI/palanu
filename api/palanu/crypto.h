/* Cryptography, run by the system: the heavy work is native, on the chip's AES, SHA and ECC blocks where it has
 * them, so an app passes buffers and handles. Handles index a table of the calling process: 8 digests (hash or HMAC) and
 * 8 ciphers (AEAD or AES-CTR). The system frees them when the app ends. Every buffer comes with its length. The
 * lengths of fixed-size values are checked: a key of 32 bytes is 32, not "up to 32". */
#ifndef PALANU_API_CRYPTO_H
#define PALANU_API_CRYPTO_H

#include "annot.h"

/* Hash algorithms for pal_hash_open and pal_hmac_open. */
#define PAL_HASH_SHA1 1
#define PAL_HASH_SHA256 2
#define PAL_HASH_SHA384 3
#define PAL_HASH_SHA512 4

/* Algorithms for pal_aead_open. The AES key length picks 128 or 256 bits. */
#define PAL_AEAD_AES128_GCM 1
#define PAL_AEAD_AES256_GCM 2
#define PAL_AEAD_CHACHA20_POLY1305 3

/* Error codes of this module: negative, as the rest of the API. */
#define PAL_ERR_UNSUPPORTED (-21)
#define PAL_ERR_AUTH (-23)

PAL_SINCE(1) PAL_WASM(random)
PAL_DOC("Fills buf with random bytes from the chip's TRNG. Returns 0, or a negative error.")
int32_t pal_random(PAL_OUT(len) void *buf, uint32_t len);

PAL_SINCE(1) PAL_WASM(hash_open)
PAL_DOC("Starts a hash with alg (PAL_HASH_*). Returns a handle, or a negative error (the table holds 8).")
int32_t pal_hash_open(int32_t alg);

PAL_SINCE(1) PAL_WASM(hmac_open)
PAL_DOC("Starts an HMAC with alg and key (any length). Feed it with pal_hash_update and finish with pal_hash_final.")
int32_t pal_hmac_open(int32_t alg, PAL_IN(klen) const void *key, uint32_t klen);

PAL_SINCE(1) PAL_WASM(hash_update)
PAL_DOC("Adds n bytes to a hash or HMAC started with pal_hash_open or pal_hmac_open. Returns 0, or a negative error.")
int32_t pal_hash_update(int32_t h, PAL_IN(n) const void *d, uint32_t n);

PAL_SINCE(1) PAL_WASM(hash_final)
PAL_DOC("Writes the digest to out (cap must hold it) and frees the handle. Returns the digest length, or a negative error.")
int32_t pal_hash_final(int32_t h, PAL_OUT(cap) void *out, uint32_t cap);

PAL_SINCE(1) PAL_WASM(aead_open)
PAL_DOC("Starts an AEAD with alg and a key of 16 (AES-128) or 32 bytes (AES-256, ChaCha20). Returns a handle.")
int32_t pal_aead_open(int32_t alg, PAL_IN(klen) const void *key, uint32_t klen);

PAL_SINCE(1) PAL_WASM(aead_seal)
PAL_DOC("Encrypts buf in place and writes the 16-byte tag. The nonce is 12 bytes and is never reused with a key.")
int32_t pal_aead_seal(int32_t h, PAL_IN(nlen) const void *nonce, uint32_t nlen, PAL_IN(alen) const void *aad,
                      uint32_t alen, PAL_OUT(len) void *buf, uint32_t len, PAL_OUT(tlen) void *tag, uint32_t tlen);

PAL_SINCE(1) PAL_WASM(aead_decrypt)
PAL_DOC("Checks the 16-byte tag and, only if it matches, decrypts buf in place. Returns 0, or PAL_ERR_AUTH with buf unchanged.")
int32_t pal_aead_decrypt(int32_t h, PAL_IN(nlen) const void *nonce, uint32_t nlen, PAL_IN(alen) const void *aad,
                         uint32_t alen, PAL_OUT(len) void *buf, uint32_t len, PAL_IN(tlen) const void *tag, uint32_t tlen);

PAL_SINCE(1) PAL_WASM(aes_ctr_open)
PAL_DOC("Starts AES-CTR with a 16 or 32 byte key and a 16-byte initial counter block. Returns a handle.")
int32_t pal_aes_ctr_open(PAL_IN(klen) const void *key, uint32_t klen, PAL_IN(ivlen) const void *iv, uint32_t ivlen);

PAL_SINCE(1) PAL_WASM(cipher_xor)
PAL_DOC("XORs buf with the next keystream bytes of a pal_aes_ctr_open handle. Calls continue the stream.")
int32_t pal_cipher_xor(int32_t h, PAL_OUT(len) void *buf, uint32_t len);

PAL_SINCE(1) PAL_WASM(chacha20_xor)
PAL_DOC("ChaCha20 keystream XOR: nlen 8 is the original 64-bit counter form (OpenSSH), 12 is RFC 8439 (32-bit counter).")
int32_t pal_chacha20_xor(PAL_IN(klen) const void *key, uint32_t klen, PAL_IN(nlen) const void *nonce, uint32_t nlen,
                         uint64_t counter, PAL_IN(ilen) const void *in, uint32_t ilen, PAL_OUT(olen) void *out, uint32_t olen);

PAL_SINCE(1) PAL_WASM(poly1305)
PAL_DOC("Poly1305 one-time authenticator: 32-byte key, 16-byte tag. The raw primitive, not the AEAD.")
int32_t pal_poly1305(PAL_IN(klen) const void *key, uint32_t klen, PAL_IN(mlen) const void *msg, uint32_t mlen,
                     PAL_OUT(tlen) void *tag, uint32_t tlen);

PAL_SINCE(1) PAL_WASM(x25519)
PAL_DOC("X25519 shared secret of scalar and point (32 bytes each). Returns NEMA_ERR_INVAL for a low-order point.")
int32_t pal_x25519(PAL_OUT(olen) void *out, uint32_t olen, PAL_IN(slen) const void *scalar, uint32_t slen,
                   PAL_IN(plen) const void *point, uint32_t plen);

PAL_SINCE(1) PAL_WASM(x25519_base)
PAL_DOC("X25519 public key of a 32-byte scalar (the base point).")
int32_t pal_x25519_base(PAL_OUT(olen) void *out, uint32_t olen, PAL_IN(slen) const void *scalar, uint32_t slen);

PAL_SINCE(1) PAL_WASM(ed25519_verify)
PAL_DOC("Verifies an Ed25519 signature (64 bytes) of msg with a 32-byte public key. Returns 0, or PAL_ERR_AUTH.")
int32_t pal_ed25519_verify(PAL_IN(pklen) const void *pk, uint32_t pklen, PAL_IN(mlen) const void *msg, uint32_t mlen,
                           PAL_IN(slen) const void *sig, uint32_t slen);

PAL_SINCE(1) PAL_WASM(ecdsa_p256_verify)
PAL_DOC("Verifies an ECDSA P-256 signature (r || s, 64 bytes) over a SHA-256 digest with a public key X || Y (64 bytes).")
int32_t pal_ecdsa_p256_verify(PAL_IN(plen) const void *pub, uint32_t plen, PAL_IN(hlen) const void *hash, uint32_t hlen,
                              PAL_IN(slen) const void *sig, uint32_t slen);

PAL_SINCE(1) PAL_WASM(rsa_verify_pkcs1)
PAL_DOC("Verifies an RSA PKCS#1 v1.5 signature. n and e are big-endian; halg is a PAL_HASH_* and hash its digest.")
int32_t pal_rsa_verify_pkcs1(PAL_IN(nlen) const void *n, uint32_t nlen, PAL_IN(elen) const void *e, uint32_t elen,
                             int32_t halg, PAL_IN(hlen) const void *hash, uint32_t hlen, PAL_IN(slen) const void *sig,
                             uint32_t slen);

#endif
