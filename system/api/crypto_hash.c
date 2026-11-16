/* The hash part of the crypto module of the palanu API (api/palanu/crypto.h): random bytes, digests and HMACs. The
 * handles index the calling process's table (crypto_tab.h). A NULL context is a built-in caller. */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "../src/crypto_mac.h"
#include "../src/crypto_tab.h"
#include "../src/cryptoimpl.h"
#include "nema/port.h"
#include "nema/types.h"

int32_t psys_api_random(api_ctx_t *c, void *buf, uint32_t len) {
    (void)c;
    nema_port_random(buf, len);
    return NEMA_OK;
}

static ct_hash_t *hash_slot(api_ctx_t *c, int32_t h) {
    crypto_tab_t *t = crypto_tab(c);
    if (!t || h < 0 || h >= CT_HASH_MAX || !t->hash[h].used) return NULL;
    return &t->hash[h];
}

static int32_t hash_new(api_ctx_t *c, int hmac, int32_t alg, const void *key, uint32_t klen) {
    if (alg < CI_HASH_SHA1 || alg > CI_HASH_SHA512) return NEMA_ERR_INVAL;
    crypto_tab_t *t = crypto_tab(c);
    if (!t) return NEMA_ERR_NOMEM;
    int i = 0;
    while (i < CT_HASH_MAX && t->hash[i].used) i++;
    if (i == CT_HASH_MAX) return NEMA_ERR_FULL;
    int rc = hmac ? crypto_mac_hmac(&t->hash[i].mac, alg, key, klen) : crypto_mac_hash(&t->hash[i].mac, alg);
    if (rc != NEMA_OK) return rc;
    t->hash[i].used = 1;
    return i;
}

int32_t psys_api_hash_open(api_ctx_t *c, int32_t alg) { return hash_new(c, 0, alg, NULL, 0); }

int32_t psys_api_hmac_open(api_ctx_t *c, int32_t alg, const void *key, uint32_t klen) {
    return hash_new(c, 1, alg, key, klen);
}

int32_t psys_api_hash_update(api_ctx_t *c, int32_t h, const void *d, uint32_t n) {
    ct_hash_t *s = hash_slot(c, h);
    if (!s) return NEMA_ERR_BADF;
    crypto_mac_update(&s->mac, d, n);
    return NEMA_OK;
}

/* The digest is written only when cap holds it; the handle is freed either way once it is written. */
int32_t psys_api_hash_final(api_ctx_t *c, int32_t h, void *out, uint32_t cap) {
    ct_hash_t *s = hash_slot(c, h);
    if (!s) return NEMA_ERR_BADF;
    size_t len = crypto_mac_len(s->mac.alg);
    if (cap < len) return NEMA_ERR_RANGE;
    crypto_mac_final(&s->mac, out);
    s->used = 0;
    return (int32_t)len;
}
