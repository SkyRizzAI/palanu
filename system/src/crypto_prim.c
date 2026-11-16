/* Adapters from the Monocypher calls to the crypto_prim.h contract: argument checks, the counter rules, and the
 * error codes. The primitives themselves are in third_party/monocypher (unmodified). */
#include <string.h>

#include "../../third_party/monocypher/monocypher.h"
#include "../../third_party/monocypher/monocypher-ed25519.h"
#include "crypto_prim.h"
#include "nema/types.h"

int prim_chacha20_xor(const uint8_t key[32], const uint8_t *nonce, size_t nlen, uint64_t counter, const uint8_t *in,
                      uint8_t *out, size_t len) {
    if (!key || !nonce || (!in && len) || (!out && len)) return NEMA_ERR_INVAL;
    if (nlen == 8) {
        crypto_chacha20_djb(out, in, len, key, nonce, counter);
        return NEMA_OK;
    }
    if (nlen == 12) {
        if (counter > 0xffffffffu) return NEMA_ERR_RANGE;
        crypto_chacha20_ietf(out, in, len, key, nonce, (uint32_t)counter);
        return NEMA_OK;
    }
    return NEMA_ERR_INVAL;
}

void prim_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]) {
    crypto_poly1305(tag, msg, len, key);
}

void prim_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                    size_t len, uint8_t tag[16]) {
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    crypto_aead_write(&ctx, buf, tag, aad, alen, buf, len);
    crypto_wipe(&ctx, sizeof(ctx));
}

int prim_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                   size_t len, const uint8_t tag[16]) {
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key, nonce);
    int bad = crypto_aead_read(&ctx, buf, tag, aad, alen, buf, len);
    crypto_wipe(&ctx, sizeof(ctx));
    return bad ? NEMA_ERR_AUTH : NEMA_OK;
}

int prim_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t acc = 0;
    crypto_x25519(out, scalar, point);
    for (int i = 0; i < 32; i++) acc |= out[i];
    return acc ? NEMA_OK : NEMA_ERR_INVAL;
}

void prim_x25519_base(uint8_t out[32], const uint8_t scalar[32]) { crypto_x25519_public_key(out, scalar); }

void prim_ed25519_keypair(uint8_t sk[64], uint8_t pk[32], const uint8_t seed[32]) {
    uint8_t s[32];
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(sk, pk, s);
    crypto_wipe(s, sizeof(s));
}

void prim_ed25519_sign(uint8_t sig[64], const uint8_t sk[64], const uint8_t *msg, size_t len) {
    crypto_ed25519_sign(sig, sk, msg, len);
}

int prim_ed25519_verify(const uint8_t pk[32], const uint8_t *msg, size_t len, const uint8_t sig[64]) {
    return crypto_ed25519_check(sig, pk, msg, len) == 0 ? NEMA_OK : NEMA_ERR_AUTH;
}
