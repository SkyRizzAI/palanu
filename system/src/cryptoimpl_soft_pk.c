/* Host side of the public-key primitives that cryptoimpl.h declares: ECDSA P-256 and RSA PKCS#1 v1.5. A portable
 * implementation of either (bignum arithmetic, a P-256 field) would be several hundred lines of code that only the
 * host tests run, so the host refuses them with NEMA_ERR_UNSUPPORTED and the tests skip them. The ESP build has them
 * through PSA (cryptoimpl_mbedtls.c). */
#ifndef ESP_PLATFORM

#include <stddef.h>
#include <stdint.h>

#include "cryptoimpl.h"
#include "nema/types.h"

int ci_ecdsa_p256_verify(const uint8_t pub[64], const uint8_t *hash, size_t hlen, const uint8_t sig[64]) {
    (void)pub, (void)hash, (void)hlen, (void)sig;
    return NEMA_ERR_UNSUPPORTED;
}

int ci_ecdsa_p256_generate(uint8_t priv[32], uint8_t pub[64]) {
    (void)priv, (void)pub;
    return NEMA_ERR_UNSUPPORTED;
}

int ci_ecdsa_p256_sign(const uint8_t priv[32], const uint8_t *hash, size_t hlen, uint8_t sig[64]) {
    (void)priv, (void)hash, (void)hlen, (void)sig;
    return NEMA_ERR_UNSUPPORTED;
}

int ci_rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg, const uint8_t *hash,
                        size_t hlen, const uint8_t *sig, size_t slen) {
    (void)n, (void)nlen, (void)e, (void)elen, (void)halg, (void)hash, (void)hlen, (void)sig, (void)slen;
    return NEMA_ERR_UNSUPPORTED;
}

#endif /* !ESP_PLATFORM */
