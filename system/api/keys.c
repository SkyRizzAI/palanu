/* The keys part of the palanu API (api/palanu/keys.h). A key's private bytes are made, loaded, or imported here and stay
 * here: the app gets handles, public blobs, and signatures. The table is the process's (crypto_tab.h); the store is
 * keystore.c. Needs the crypto.keys permission (the glue checks it before these run). */
#include <stdint.h>
#include <string.h>

#include "../../third_party/monocypher/monocypher.h"
#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "../src/crypto_mac.h"
#include "../src/crypto_prim.h"
#include "../src/crypto_tab.h"
#include "../src/cryptoimpl.h"
#include "../src/keystore.h"
#include "nema/port.h"
#include "nema/types.h"

static int name_of(const char *p, uint32_t n, char out[KS_NAME_MAX + 1]) {
    if (!ks_name_ok(p, n)) return NEMA_ERR_INVAL;
    memcpy(out, p, n);
    out[n] = 0;
    return NEMA_OK;
}

static int32_t free_key_slot(api_ctx_t *c) {
    crypto_tab_t *t = crypto_tab(c);
    if (!t) return NEMA_ERR_NOMEM;
    for (int i = 0; i < CT_KEY_MAX; i++)
        if (!t->key[i].used) return i;
    return NEMA_ERR_FULL;
}

/* Fills a free slot from key material (layout of KS_PLAIN_LEN): secret[64] then pub[65]. */
static int32_t key_add(api_ctx_t *c, uint8_t type, const uint8_t plain[KS_PLAIN_LEN]) {
    int32_t i = free_key_slot(c);
    if (i < 0) return i;
    ct_key_t *k = &crypto_tab(c)->key[i];
    k->type = type;
    memcpy(k->secret, plain, 64);
    memcpy(k->pub, plain + 64, 65);
    k->used = 1;
    return i;
}

static ct_key_t *key_of(api_ctx_t *c, int32_t h) {
    crypto_tab_t *t = crypto_tab(c);
    if (!t || h < 0 || h >= CT_KEY_MAX || !t->key[h].used) return NULL;
    return &t->key[h];
}

/* Stores a new key and loads it. The table slot is checked first, so a full table never leaves a stored key behind. */
static int32_t store_new(api_ctx_t *c, const char *nm, uint8_t type, uint8_t plain[KS_PLAIN_LEN]) {
    if (ks_exists(nm)) return NEMA_ERR_EXIST;
    int32_t slot = free_key_slot(c);
    if (slot < 0) return slot;
    int rc = ks_save(nm, type, plain);
    if (rc != NEMA_OK) return rc;
    return key_add(c, type, plain);
}

int32_t psys_api_key_generate(api_ctx_t *c, int32_t type, const char *name, uint32_t nlen) {
    char nm[KS_NAME_MAX + 1];
    int rc = name_of(name, nlen, nm);
    if (rc != NEMA_OK) return rc;
    if (type != CT_KEY_ED25519 && type != CT_KEY_ECDSA_P256) return NEMA_ERR_UNSUPPORTED;
    if (ks_exists(nm)) return NEMA_ERR_EXIST;
    uint8_t plain[KS_PLAIN_LEN];
    memset(plain, 0, sizeof(plain));
    if (type == CT_KEY_ED25519) {
        uint8_t seed[32];
        nema_port_random(seed, sizeof(seed));
        prim_ed25519_keypair(plain, plain + 64, seed); /* secret: seed || pub, as Monocypher stores it */
        crypto_wipe(seed, sizeof(seed));
    } else {
        uint8_t priv[32], pub[64];
        rc = ci_ecdsa_p256_generate(priv, pub);
        if (rc != NEMA_OK) return rc;
        memcpy(plain, priv, 32);
        memcpy(plain + 64, pub, 64);
        crypto_wipe(priv, sizeof(priv));
    }
    int32_t h = store_new(c, nm, (uint8_t)type, plain);
    crypto_wipe(plain, sizeof(plain));
    return h;
}

int32_t psys_api_key_load(api_ctx_t *c, const char *name, uint32_t nlen) {
    char nm[KS_NAME_MAX + 1];
    int rc = name_of(name, nlen, nm);
    if (rc != NEMA_OK) return rc;
    int32_t slot = free_key_slot(c);
    if (slot < 0) return slot;
    uint8_t type = 0, plain[KS_PLAIN_LEN];
    rc = ks_load(nm, &type, plain);
    if (rc != NEMA_OK) return rc;
    int32_t h = key_add(c, type, plain);
    crypto_wipe(plain, sizeof(plain));
    return h;
}

/* The passphrase is accepted for encrypted keys, which are not supported yet; an unencrypted key ignores it. */
int32_t psys_api_key_import(api_ctx_t *c, const void *blob, uint32_t blen, const char *pass, uint32_t plen,
                            const char *name, uint32_t nlen) {
    (void)pass, (void)plen;
    char nm[KS_NAME_MAX + 1];
    int rc = name_of(name, nlen, nm);
    if (rc != NEMA_OK) return rc;
    if (!blob || blen == 0) return NEMA_ERR_INVAL;
    uint8_t type = 0, plain[KS_PLAIN_LEN];
    rc = kf_import_openssh(blob, blen, &type, plain);
    if (rc != NEMA_OK) return rc;
    int32_t h = store_new(c, nm, type, plain);
    crypto_wipe(plain, sizeof(plain));
    return h;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* The SSH wire form of the public key: "ssh-ed25519" and 32 bytes, or ecdsa-sha2-nistp256 with the uncompressed point. */
static size_t public_blob(const ct_key_t *k, uint8_t *b) {
    static const char ed[] = "ssh-ed25519";
    static const char ec[] = "ecdsa-sha2-nistp256";
    static const char curve[] = "nistp256";
    size_t n = 0;
    if (k->type == CT_KEY_ED25519) {
        put_u32(b, sizeof(ed) - 1);
        memcpy(b + 4, ed, sizeof(ed) - 1);
        n = 4 + sizeof(ed) - 1;
        put_u32(b + n, 32);
        memcpy(b + n + 4, k->pub, 32);
        n += 4 + 32;
    } else {
        put_u32(b, sizeof(ec) - 1);
        memcpy(b + 4, ec, sizeof(ec) - 1);
        n = 4 + sizeof(ec) - 1;
        put_u32(b + n, sizeof(curve) - 1);
        memcpy(b + n + 4, curve, sizeof(curve) - 1);
        n += 4 + sizeof(curve) - 1;
        put_u32(b + n, 65);
        b[n + 4] = 0x04;
        memcpy(b + n + 5, k->pub, 64);
        n += 4 + 65;
    }
    return n;
}

int32_t psys_api_key_public(api_ctx_t *c, int32_t h, void *out, uint32_t cap) {
    ct_key_t *k = key_of(c, h);
    if (!k) return NEMA_ERR_BADF;
    uint8_t blob[128];
    size_t n = public_blob(k, blob);
    if (cap < n) return NEMA_ERR_RANGE;
    memcpy(out, blob, n);
    return (int32_t)n;
}

/* ed25519 signs the data itself. ecdsa signs its SHA-256 digest, as the SSH ecdsa-sha2-nistp256 method does. */
int32_t psys_api_key_sign(api_ctx_t *c, int32_t h, const void *data, uint32_t len, void *sig, uint32_t cap) {
    ct_key_t *k = key_of(c, h);
    if (!k) return NEMA_ERR_BADF;
    if (cap < 64) return NEMA_ERR_RANGE;
    if (k->type == CT_KEY_ED25519) {
        prim_ed25519_sign(sig, k->secret, data, len);
        return 64;
    }
    crypto_mac_t m;
    uint8_t digest[32];
    crypto_mac_hash(&m, CI_HASH_SHA256);
    crypto_mac_update(&m, data, len);
    crypto_mac_final(&m, digest);
    int rc = ci_ecdsa_p256_sign(k->secret, digest, sizeof(digest), sig);
    crypto_wipe(digest, sizeof(digest));
    return rc == NEMA_OK ? 64 : rc;
}

int32_t psys_api_key_list(api_ctx_t *c, void *out, uint32_t cap) {
    (void)c;
    return ks_list((char *)out, cap);
}

int32_t psys_api_key_delete(api_ctx_t *c, const char *name, uint32_t nlen) {
    (void)c;
    char nm[KS_NAME_MAX + 1];
    int rc = name_of(name, nlen, nm);
    if (rc != NEMA_OK) return rc;
    if (!ks_exists(nm)) return NEMA_ERR_NOENT;
    return ks_delete(nm);
}
