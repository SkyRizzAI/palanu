/* The symmetric and curve primitives that are the same on every build: ChaCha20, Poly1305, ChaCha20-Poly1305 (RFC 8439),
 * X25519 (RFC 7748) and Ed25519 (RFC 8032), from Monocypher (third_party/monocypher). Hardware does not help these on
 * the chip, so there is no per-platform split. */
#ifndef SYSTEM_CRYPTO_PRIM_H
#define SYSTEM_CRYPTO_PRIM_H

#include <stddef.h>
#include <stdint.h>

/* ChaCha20 keystream XOR. nlen 8: the original 64-bit counter and nonce (the OpenSSH stream form). nlen 12: RFC 8439
 * (32-bit counter; the counter must fit 32 bits). in and out may be the same buffer. Returns 0, or NEMA_ERR_INVAL. */
int prim_chacha20_xor(const uint8_t key[32], const uint8_t *nonce, size_t nlen, uint64_t counter, const uint8_t *in,
                      uint8_t *out, size_t len);
void prim_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]);

/* RFC 8439 AEAD. seal encrypts buf in place. open decrypts in place only when the tag matches; otherwise it returns
 * NEMA_ERR_AUTH and buf is unchanged. */
void prim_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                    size_t len, uint8_t tag[16]);
int prim_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                   size_t len, const uint8_t tag[16]);

/* X25519. Returns NEMA_ERR_INVAL when the shared secret is all zero (a low-order point), as RFC 7748 section 6.1 asks. */
int prim_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void prim_x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* Ed25519. The secret key is 64 bytes: the 32-byte seed followed by the public key, as Monocypher stores it. */
void prim_ed25519_keypair(uint8_t sk[64], uint8_t pk[32], const uint8_t seed[32]);
void prim_ed25519_sign(uint8_t sig[64], const uint8_t sk[64], const uint8_t *msg, size_t len);
int prim_ed25519_verify(const uint8_t pk[32], const uint8_t *msg, size_t len, const uint8_t sig[64]);

#endif
