/* ESP implementation of cryptoimpl.h on PSA Crypto (TF-PSA-Crypto in ESP-IDF). The builtin drivers use the chip's AES,
 * SHA and ECC blocks when the IDF configuration enables them, so the calls here need no hardware code.
 *
 * Not verified on the board yet: this file is only syntax-checked for the ESP toolchain. Compiled only when
 * ESP_PLATFORM is defined; the host has cryptoimpl_soft*.c. */
#ifdef ESP_PLATFORM

#include <string.h>

#include "cryptoimpl.h"
#include "nema/mem.h"
#include "nema/proc.h"
#include "nema/types.h"
#include "psa/crypto.h"

typedef struct {
    uint8_t alg;
    psa_hash_operation_t op;
} psa_hash_state_t;

_Static_assert(sizeof(psa_hash_state_t) <= CI_HASH_STATE_MAX, "PSA hash state does not fit cryptoimpl.h");

typedef struct {
    uint8_t kind; /* 1 GCM (one-shot per call), 2 CTR (streaming operation) */
    uint8_t klen;
    uint8_t key[32];
    psa_cipher_operation_t ctr;
} psa_cipher_state_t;

_Static_assert(sizeof(psa_cipher_state_t) <= CI_CIPHER_STATE_MAX, "PSA cipher state does not fit cryptoimpl.h");

enum { KIND_GCM = 1, KIND_CTR = 2 };

static int ready(void) { return psa_crypto_init() == PSA_SUCCESS ? NEMA_OK : NEMA_ERR_UNSUPPORTED; }

static int map(psa_status_t s) {
    switch (s) {
    case PSA_SUCCESS: return NEMA_OK;
    case PSA_ERROR_INVALID_SIGNATURE: return NEMA_ERR_AUTH;
    case PSA_ERROR_NOT_SUPPORTED: return NEMA_ERR_UNSUPPORTED;
    case PSA_ERROR_INSUFFICIENT_MEMORY: return NEMA_ERR_NOMEM;
    case PSA_ERROR_BUFFER_TOO_SMALL:
    case PSA_ERROR_INVALID_ARGUMENT: return NEMA_ERR_INVAL;
    default: return NEMA_ERR_IO;
    }
}

static psa_algorithm_t hash_alg(int alg) {
    switch (alg) {
    case CI_HASH_SHA1: return PSA_ALG_SHA_1;
    case CI_HASH_SHA256: return PSA_ALG_SHA_256;
    case CI_HASH_SHA384: return PSA_ALG_SHA_384;
    case CI_HASH_SHA512: return PSA_ALG_SHA_512;
    default: return 0;
    }
}

size_t ci_hash_len(int alg) {
    switch (alg) {
    case CI_HASH_SHA1: return 20;
    case CI_HASH_SHA256: return 32;
    case CI_HASH_SHA384: return 48;
    case CI_HASH_SHA512: return 64;
    default: return 0;
    }
}

size_t ci_hash_block(int alg) {
    switch (alg) {
    case CI_HASH_SHA1:
    case CI_HASH_SHA256: return 64;
    case CI_HASH_SHA384:
    case CI_HASH_SHA512: return 128;
    default: return 0;
    }
}

int ci_hash_init(ci_hash_t *h, int alg) {
    psa_hash_state_t *s = (psa_hash_state_t *)h;
    memset(s, 0, sizeof(*s));
    if (hash_alg(alg) == 0 || ready() != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
    s->op = psa_hash_operation_init();
    if (psa_hash_setup(&s->op, hash_alg(alg)) != PSA_SUCCESS) return NEMA_ERR_UNSUPPORTED;
    s->alg = (uint8_t)alg;
    return NEMA_OK;
}

void ci_hash_update(ci_hash_t *h, const void *d, size_t n) {
    psa_hash_state_t *s = (psa_hash_state_t *)h;
    if (s->alg && n) psa_hash_update(&s->op, d, n);
}

void ci_hash_final(ci_hash_t *h, uint8_t *out) {
    psa_hash_state_t *s = (psa_hash_state_t *)h;
    uint8_t full[64];
    size_t got = 0;
    if (s->alg) psa_hash_finish(&s->op, full, sizeof(full), &got);
    if (got) memcpy(out, full, ci_hash_len(s->alg));
    memset(s, 0, sizeof(*s));
    memset(full, 0, sizeof(full));
}

/* ---- AES-GCM ----
 * The seal and open work on a scratch copy (ciphertext || tag) so the caller's buffer is untouched when the tag does not
 * match, and so PSA never sees overlapping buffers. The scratch is charged to the calling process. */

static psa_status_t gcm_key(const uint8_t *key, size_t klen, psa_key_id_t *kid, psa_key_usage_t usage) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, klen * 8);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_usage_flags(&attr, usage);
    return psa_import_key(&attr, key, klen, kid);
}

int ci_gcm_init(ci_cipher_t *c, const uint8_t *key, size_t klen) {
    psa_cipher_state_t *s = (psa_cipher_state_t *)c;
    memset(s, 0, sizeof(*s));
    if (!key || (klen != 16 && klen != 32)) return NEMA_ERR_INVAL;
    if (ready() != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
    memcpy(s->key, key, klen);
    s->klen = (uint8_t)klen;
    s->kind = KIND_GCM;
    return NEMA_OK;
}

/* Input and output are separate regions of one scratch block: input at 0 (ciphertext || tag for open), output after it. */
static int gcm_run(psa_cipher_state_t *s, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                   size_t len, uint8_t tag[16], int seal) {
    if (!nonce || (!buf && len) || (!aad && alen)) return NEMA_ERR_INVAL;
    if (len > (size_t)1 << 20) return NEMA_ERR_RANGE;
    nema_proc_t *p = nema_self();
    size_t cap = 2 * (len + 16);
    uint8_t *scratch = nema_alloc(NEMA_MEM_ANY, p ? p->owner : NEMA_OWNER_KERNEL, cap);
    if (!scratch) return NEMA_ERR_NOMEM;
    uint8_t *out = scratch + len + 16;
    psa_key_id_t kid = 0;
    size_t out_len = 0;
    int rc;
    if (seal) {
        memcpy(scratch, buf, len);
        rc = map(gcm_key(s->key, s->klen, &kid, PSA_KEY_USAGE_ENCRYPT));
        if (rc == NEMA_OK) {
            rc = map(psa_aead_encrypt(kid, PSA_ALG_GCM, nonce, 12, aad, alen, scratch, len, out, len + 16, &out_len));
            psa_destroy_key(kid);
        }
        if (rc == NEMA_OK && out_len != len + 16) rc = NEMA_ERR_IO;
        if (rc == NEMA_OK) {
            memcpy(buf, out, len);
            memcpy(tag, out + len, 16);
        }
    } else {
        memcpy(scratch, buf, len);
        memcpy(scratch + len, tag, 16);
        rc = map(gcm_key(s->key, s->klen, &kid, PSA_KEY_USAGE_DECRYPT));
        if (rc == NEMA_OK) {
            rc = map(psa_aead_decrypt(kid, PSA_ALG_GCM, nonce, 12, aad, alen, scratch, len + 16, out, len + 16, &out_len));
            psa_destroy_key(kid);
        }
        if (rc == NEMA_OK && out_len != len) rc = NEMA_ERR_IO;
        if (rc == NEMA_OK) memcpy(buf, out, len);
    }
    memset(scratch, 0, cap);
    nema_free(scratch);
    return rc;
}

int ci_gcm_seal(ci_cipher_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf, size_t len,
                uint8_t tag[16]) {
    psa_cipher_state_t *s = (psa_cipher_state_t *)c;
    if (s->kind != KIND_GCM) return NEMA_ERR_STATE;
    return gcm_run(s, nonce, aad, alen, buf, len, tag, 1);
}

int ci_gcm_open(ci_cipher_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf, size_t len,
                const uint8_t tag[16]) {
    psa_cipher_state_t *s = (psa_cipher_state_t *)c;
    if (s->kind != KIND_GCM) return NEMA_ERR_STATE;
    uint8_t t[16];
    memcpy(t, tag, 16);
    return gcm_run(s, nonce, aad, alen, buf, len, t, 0);
}

/* ---- AES-CTR ----
 * The operation keeps its key material, so the key slot is destroyed right after setup and nothing is left to free
 * when the table is swept. */

int ci_ctr_init(ci_cipher_t *c, const uint8_t *key, size_t klen, const uint8_t iv[16]) {
    psa_cipher_state_t *s = (psa_cipher_state_t *)c;
    memset(s, 0, sizeof(*s));
    if (!key || !iv || (klen != 16 && klen != 32)) return NEMA_ERR_INVAL;
    if (ready() != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t kid = 0;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, klen * 8);
    psa_set_key_algorithm(&attr, PSA_ALG_CTR);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    if (psa_import_key(&attr, key, klen, &kid) != PSA_SUCCESS) return NEMA_ERR_INVAL;
    s->ctr = psa_cipher_operation_init();
    psa_status_t st = psa_cipher_encrypt_setup(&s->ctr, kid, PSA_ALG_CTR);
    psa_destroy_key(kid); /* the operation holds what it needs */
    if (st != PSA_SUCCESS) return map(st);
    st = psa_cipher_set_iv(&s->ctr, iv, 16);
    if (st != PSA_SUCCESS) return map(st);
    s->kind = KIND_CTR;
    return NEMA_OK;
}

void ci_ctr_xor(ci_cipher_t *c, uint8_t *buf, size_t len) {
    psa_cipher_state_t *s = (psa_cipher_state_t *)c;
    if (s->kind != KIND_CTR) return;
    uint8_t in[64], out[64 + 16];
    while (len > 0) {
        size_t take = len < 64 ? len : 64;
        memset(in, 0, take);
        size_t got = 0;
        if (psa_cipher_update(&s->ctr, in, take, out, sizeof(out), &got) != PSA_SUCCESS || got < take) return;
        for (size_t i = 0; i < take; i++) buf[i] ^= out[i];
        buf += take, len -= take;
    }
}

/* ---- ECDSA P-256 ---- */

static psa_status_t ecc_import(const uint8_t *data, size_t n, psa_key_type_t type, psa_key_usage_t usage,
                               psa_key_id_t *kid) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, type);
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, usage);
    return psa_import_key(&attr, data, n, kid);
}

int ci_ecdsa_p256_verify(const uint8_t pub[64], const uint8_t *hash, size_t hlen, const uint8_t sig[64]) {
    if (hlen != 32) return NEMA_ERR_INVAL;
    if (ready() != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
    uint8_t point[65];
    point[0] = 0x04; /* uncompressed point */
    memcpy(point + 1, pub, 64);
    psa_key_id_t kid = 0;
    int rc = map(ecc_import(point, sizeof(point), PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1), PSA_KEY_USAGE_VERIFY_HASH, &kid));
    if (rc != NEMA_OK) return rc;
    rc = map(psa_verify_hash(kid, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hlen, sig, 64));
    psa_destroy_key(kid);
    return rc;
}

int ci_ecdsa_p256_generate(uint8_t priv[32], uint8_t pub[64]) {
    if (ready() != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_key_id_t kid = 0;
    if (psa_generate_key(&attr, &kid) != PSA_SUCCESS) return NEMA_ERR_IO;
    uint8_t point[65];
    size_t pl = 0, el = 0;
    int rc = map(psa_export_key(kid, priv, 32, &el));
    if (rc == NEMA_OK) rc = map(psa_export_public_key(kid, point, sizeof(point), &pl));
    psa_destroy_key(kid);
    if (rc == NEMA_OK && (el != 32 || pl != 65)) rc = NEMA_ERR_IO;
    if (rc == NEMA_OK) memcpy(pub, point + 1, 64);
    return rc;
}

int ci_ecdsa_p256_sign(const uint8_t priv[32], const uint8_t *hash, size_t hlen, uint8_t sig[64]) {
    if (hlen != 32) return NEMA_ERR_INVAL;
    if (ready() != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
    psa_key_id_t kid = 0;
    int rc = map(ecc_import(priv, 32, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), PSA_KEY_USAGE_SIGN_HASH, &kid));
    if (rc != NEMA_OK) return rc;
    size_t sl = 0;
    rc = map(psa_sign_hash(kid, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hlen, sig, 64, &sl));
    psa_destroy_key(kid);
    if (rc == NEMA_OK && sl != 64) rc = NEMA_ERR_IO;
    return rc;
}

/* ---- RSA PKCS#1 v1.5 ---- */

/* Appends a DER length. Returns the bytes used. */
static size_t der_len(uint8_t *p, size_t n) {
    if (n < 128) {
        p[0] = (uint8_t)n;
        return 1;
    }
    if (n < 256) {
        p[0] = 0x81, p[1] = (uint8_t)n;
        return 2;
    }
    p[0] = 0x82, p[1] = (uint8_t)(n >> 8), p[2] = (uint8_t)n;
    return 3;
}

/* DER INTEGER for a big-endian unsigned value (a leading zero keeps it positive). Returns the bytes written. */
static size_t der_int(uint8_t *out, const uint8_t *v, size_t n) {
    while (n > 1 && v[0] == 0) v++, n--;
    int pad = v[0] & 0x80 ? 1 : 0;
    size_t body = n + (size_t)pad;
    out[0] = 0x02;
    size_t k = der_len(out + 1, body);
    size_t off = 1 + k;
    if (pad) out[off++] = 0;
    memcpy(out + off, v, n);
    return off + n;
}

/* The RSAPublicKey structure of PKCS#1 (RFC 8017 A.1.1), which PSA imports directly. */
#define RSA_DER_MAX 1280

static size_t rsa_public_der(uint8_t *out, const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen) {
    uint8_t body[RSA_DER_MAX];
    size_t bl = der_int(body, n, nlen);
    bl += der_int(body + bl, e, elen);
    out[0] = 0x30;
    size_t k = der_len(out + 1, bl);
    memcpy(out + 1 + k, body, bl);
    return 1 + k + bl;
}

int ci_rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg, const uint8_t *hash,
                        size_t hlen, const uint8_t *sig, size_t slen) {
    if (!n || !e || !hash || !sig || nlen == 0 || elen == 0) return NEMA_ERR_INVAL;
    psa_algorithm_t h = hash_alg(halg);
    if (h == 0 || hlen != ci_hash_len(halg)) return NEMA_ERR_INVAL;
    if (nlen + elen + 16 > RSA_DER_MAX - 16) return NEMA_ERR_RANGE;
    if (ready() != NEMA_OK) return NEMA_ERR_UNSUPPORTED;
    uint8_t der[RSA_DER_MAX + 8];
    size_t dl = rsa_public_der(der, n, nlen, e, elen);
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_algorithm(&attr, PSA_ALG_RSA_PKCS1V15_SIGN(h));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_key_id_t kid = 0;
    int rc = map(psa_import_key(&attr, der, dl, &kid));
    if (rc != NEMA_OK) return rc;
    rc = map(psa_verify_hash(kid, PSA_ALG_RSA_PKCS1V15_SIGN(h), hash, hlen, sig, slen));
    psa_destroy_key(kid);
    return rc;
}

#endif /* ESP_PLATFORM */
