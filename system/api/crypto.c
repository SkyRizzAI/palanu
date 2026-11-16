/* The cipher and public-key part of the crypto module of the palanu API (api/palanu/crypto.h). Every fixed-size value is
 * checked against its length before any work. Handles: the process's cipher table (crypto_tab.h). */
#include <stdint.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "../src/crypto_prim.h"
#include "../src/crypto_tab.h"
#include "../src/cryptoimpl.h"
#include "nema/types.h"

#define NONCE_LEN 12
#define TAG_LEN 16

static ct_cipher_t *cipher_slot(api_ctx_t *c, int32_t h, int kind) {
    crypto_tab_t *t = crypto_tab(c);
    if (!t || h < 0 || h >= CT_CIPHER_MAX || !t->cipher[h].used || t->cipher[h].kind != kind) return NULL;
    return &t->cipher[h];
}

static int32_t cipher_free_slot(api_ctx_t *c) {
    crypto_tab_t *t = crypto_tab(c);
    if (!t) return NEMA_ERR_NOMEM;
    for (int i = 0; i < CT_CIPHER_MAX; i++)
        if (!t->cipher[i].used) return i;
    return NEMA_ERR_FULL;
}

int32_t psys_api_aead_open(api_ctx_t *c, int32_t alg, const void *key, uint32_t klen) {
    size_t want;
    if (alg == CI_AEAD_AES128_GCM) want = 16;
    else if (alg == CI_AEAD_AES256_GCM || alg == CI_AEAD_CHACHA20_POLY1305) want = 32;
    else return NEMA_ERR_INVAL;
    if (klen != want) return NEMA_ERR_INVAL;
    int32_t i = cipher_free_slot(c);
    if (i < 0) return i;
    ct_cipher_t *s = &crypto_tab(c)->cipher[i];
    if (alg == CI_AEAD_CHACHA20_POLY1305) {
        memcpy(s->key, key, 32);
    } else {
        int rc = ci_gcm_init(&s->c, key, klen);
        if (rc != NEMA_OK) return rc;
    }
    s->kind = CT_AEAD;
    s->alg = (uint8_t)alg;
    s->used = 1;
    return i;
}

int32_t psys_api_aead_seal(api_ctx_t *c, int32_t h, const void *nonce, uint32_t nlen, const void *aad, uint32_t alen,
                           void *buf, uint32_t len, void *tag, uint32_t tlen) {
    ct_cipher_t *s = cipher_slot(c, h, CT_AEAD);
    if (!s) return NEMA_ERR_BADF;
    if (nlen != NONCE_LEN || tlen != TAG_LEN) return NEMA_ERR_INVAL;
    if (s->alg == CI_AEAD_CHACHA20_POLY1305) {
        prim_aead_seal(s->key, nonce, aad, alen, buf, len, tag);
        return NEMA_OK;
    }
    return ci_gcm_seal(&s->c, nonce, aad, alen, buf, len, tag);
}

int32_t psys_api_aead_decrypt(api_ctx_t *c, int32_t h, const void *nonce, uint32_t nlen, const void *aad, uint32_t alen,
                              void *buf, uint32_t len, const void *tag, uint32_t tlen) {
    ct_cipher_t *s = cipher_slot(c, h, CT_AEAD);
    if (!s) return NEMA_ERR_BADF;
    if (nlen != NONCE_LEN || tlen != TAG_LEN) return NEMA_ERR_INVAL;
    if (s->alg == CI_AEAD_CHACHA20_POLY1305) return prim_aead_open(s->key, nonce, aad, alen, buf, len, tag);
    return ci_gcm_open(&s->c, nonce, aad, alen, buf, len, tag);
}

int32_t psys_api_aes_ctr_open(api_ctx_t *c, const void *key, uint32_t klen, const void *iv, uint32_t ivlen) {
    if (klen != 16 && klen != 32) return NEMA_ERR_INVAL;
    if (ivlen != 16) return NEMA_ERR_INVAL;
    int32_t i = cipher_free_slot(c);
    if (i < 0) return i;
    ct_cipher_t *s = &crypto_tab(c)->cipher[i];
    int rc = ci_ctr_init(&s->c, key, klen, iv);
    if (rc != NEMA_OK) return rc;
    s->kind = CT_CTR;
    s->alg = 0;
    s->used = 1;
    return i;
}

int32_t psys_api_cipher_xor(api_ctx_t *c, int32_t h, void *buf, uint32_t len) {
    ct_cipher_t *s = cipher_slot(c, h, CT_CTR);
    if (!s) return NEMA_ERR_BADF;
    ci_ctr_xor(&s->c, buf, len);
    return NEMA_OK;
}

int32_t psys_api_chacha20_xor(api_ctx_t *c, const void *key, uint32_t klen, const void *nonce, uint32_t nlen,
                              uint64_t counter, const void *in, uint32_t ilen, void *out, uint32_t olen) {
    (void)c;
    if (klen != 32 || ilen != olen) return NEMA_ERR_INVAL;
    return prim_chacha20_xor(key, nonce, nlen, counter, in, out, ilen);
}

int32_t psys_api_poly1305(api_ctx_t *c, const void *key, uint32_t klen, const void *msg, uint32_t mlen, void *tag,
                          uint32_t tlen) {
    (void)c;
    if (klen != 32 || tlen != TAG_LEN) return NEMA_ERR_INVAL;
    prim_poly1305(key, msg, mlen, tag);
    return NEMA_OK;
}

int32_t psys_api_x25519(api_ctx_t *c, void *out, uint32_t olen, const void *scalar, uint32_t slen, const void *point,
                        uint32_t plen) {
    (void)c;
    if (olen != 32 || slen != 32 || plen != 32) return NEMA_ERR_INVAL;
    return prim_x25519(out, scalar, point);
}

int32_t psys_api_x25519_base(api_ctx_t *c, void *out, uint32_t olen, const void *scalar, uint32_t slen) {
    (void)c;
    if (olen != 32 || slen != 32) return NEMA_ERR_INVAL;
    prim_x25519_base(out, scalar);
    return NEMA_OK;
}

int32_t psys_api_ed25519_verify(api_ctx_t *c, const void *pk, uint32_t pklen, const void *msg, uint32_t mlen,
                                const void *sig, uint32_t slen) {
    (void)c;
    if (pklen != 32 || slen != 64) return NEMA_ERR_INVAL;
    return prim_ed25519_verify(pk, msg, mlen, sig);
}

int32_t psys_api_ecdsa_p256_verify(api_ctx_t *c, const void *pub, uint32_t plen, const void *hash, uint32_t hlen,
                                   const void *sig, uint32_t slen) {
    (void)c;
    if (plen != 64 || hlen != 32 || slen != 64) return NEMA_ERR_INVAL;
    return ci_ecdsa_p256_verify(pub, hash, hlen, sig);
}

int32_t psys_api_rsa_verify_pkcs1(api_ctx_t *c, const void *n, uint32_t nlen, const void *e, uint32_t elen, int32_t halg,
                                  const void *hash, uint32_t hlen, const void *sig, uint32_t slen) {
    (void)c;
    if (halg < CI_HASH_SHA1 || halg > CI_HASH_SHA512 || nlen == 0 || elen == 0 || slen == 0) return NEMA_ERR_INVAL;
    return ci_rsa_verify_pkcs1(n, nlen, e, elen, halg, hash, hlen, sig, slen);
}
