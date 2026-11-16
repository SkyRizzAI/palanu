/* Digest and HMAC state (RFC 2104), over whichever hash primitive cryptoimpl.h provides on this build. */
#include <string.h>

#include "crypto_mac.h"
#include "nema/types.h"
#include "../../third_party/monocypher/monocypher.h"

size_t crypto_mac_len(int alg) { return ci_hash_len(alg); }

int crypto_mac_hash(crypto_mac_t *m, int alg) {
    if (ci_hash_len(alg) == 0) return NEMA_ERR_UNSUPPORTED;
    if (ci_hash_init(&m->in, alg) != 0) return NEMA_ERR_UNSUPPORTED;
    m->alg = (uint8_t)alg;
    m->hmac = 0;
    return NEMA_OK;
}

/* Keys longer than the block are hashed first; shorter ones are zero padded (RFC 2104 section 2). */
int crypto_mac_hmac(crypto_mac_t *m, int alg, const uint8_t *key, size_t klen) {
    size_t block = ci_hash_block(alg);
    if (block == 0 || (klen && !key)) return NEMA_ERR_INVAL;
    uint8_t k[128];
    uint8_t pad[128];
    memset(k, 0, sizeof(k));
    if (klen > block) {
        crypto_mac_t t;
        if (crypto_mac_hash(&t, alg) != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
        crypto_mac_update(&t, key, klen);
        crypto_mac_final(&t, k);
    } else if (klen) {
        memcpy(k, key, klen);
    }
    if (ci_hash_init(&m->in, alg) != 0 || ci_hash_init(&m->out, alg) != 0) return NEMA_ERR_UNSUPPORTED;
    for (size_t i = 0; i < block; i++) pad[i] = k[i] ^ 0x36;
    ci_hash_update(&m->in, pad, block);
    for (size_t i = 0; i < block; i++) pad[i] = k[i] ^ 0x5c;
    ci_hash_update(&m->out, pad, block);
    m->alg = (uint8_t)alg;
    m->hmac = 1;
    crypto_wipe(k, sizeof(k));
    crypto_wipe(pad, sizeof(pad));
    return NEMA_OK;
}

void crypto_mac_update(crypto_mac_t *m, const void *d, size_t n) {
    if (m->alg) ci_hash_update(&m->in, d, n);
}

size_t crypto_mac_final(crypto_mac_t *m, uint8_t *out) {
    size_t len = ci_hash_len(m->alg);
    if (!m->alg) return 0;
    uint8_t inner[64];
    ci_hash_final(&m->in, inner);
    if (m->hmac) {
        ci_hash_update(&m->out, inner, len);
        ci_hash_final(&m->out, out);
    } else {
        memcpy(out, inner, len);
    }
    crypto_wipe(inner, sizeof(inner));
    m->alg = 0;
    return len;
}
