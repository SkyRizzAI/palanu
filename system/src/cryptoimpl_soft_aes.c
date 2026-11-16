/* Host AES-128/256 (FIPS 197, encryption only: GCM and CTR never decrypt a block), GCM (NIST SP 800-38D, 12-byte
 * nonces) and CTR. Portable C, byte oriented: slow, and meant as the reference for the host tests. ESP uses the chip's
 * AES through cryptoimpl_mbedtls.c. */
#ifndef ESP_PLATFORM

#include <string.h>

#include "cryptoimpl.h"
#include "nema/types.h"
#include "../../third_party/monocypher/monocypher.h"

enum { KIND_NONE = 0, KIND_GCM = 1, KIND_CTR = 2 };

typedef struct {
    uint8_t rk[240]; /* round keys, up to 15 rounds of 16 bytes */
    uint8_t nr;
    uint8_t h[16]; /* GHASH key: E(K, 0) */
} soft_gcm_t;

typedef struct {
    uint8_t rk[240];
    uint8_t nr;
    uint8_t ctr[16];
    uint8_t ks[16]; /* keystream block for the current counter */
    uint8_t used;   /* bytes of ks already consumed; 16 = none buffered */
} soft_ctr_t;

typedef struct {
    uint8_t kind;
    union {
        soft_gcm_t gcm;
        soft_ctr_t ctr;
    } u;
} soft_cipher_t;

_Static_assert(sizeof(soft_cipher_t) <= CI_CIPHER_STATE_MAX, "soft cipher state does not fit cryptoimpl.h");

static uint8_t SBOX[256];
static int s_sbox_ready;

#define ROTL8(x, n) ((uint8_t)(((x) << (n)) | ((x) >> (8 - (n)))))

/* The S-box is the multiplicative inverse in GF(2^8) followed by the affine map of FIPS 197 section 5.1.1, computed
 * here rather than typed in. */
static void sbox_init(void) {
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0)); /* p *= 3 */
        q ^= (uint8_t)(q << 1);                                 /* q /= 3 */
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ROTL8(q, 1) ^ ROTL8(q, 2) ^ ROTL8(q, 3) ^ ROTL8(q, 4));
        SBOX[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    SBOX[0] = 0x63;
    s_sbox_ready = 1;
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

/* nk words of key, nr rounds (10 for AES-128, 14 for AES-256). */
static void aes_expand(uint8_t *rk, const uint8_t *key, int nk, int nr) {
    if (!s_sbox_ready) sbox_init();
    memcpy(rk, key, (size_t)4 * nk);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (nr + 1); i++) {
        uint8_t t[4];
        memcpy(t, rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t a = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[a];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
        }
        for (int j = 0; j < 4; j++) rk[4 * i + j] = (uint8_t)(rk[4 * (i - nk) + j] ^ t[j]);
    }
}

static void aes_encrypt(const uint8_t *rk, int nr, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= nr; r++) {
        uint8_t t[16];
        /* SubBytes and ShiftRows: byte (row, col) moves to (row, col - row). State is column major. */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[4 * c + row] = SBOX[s[4 * ((c + row) % 4) + row]];
        if (r != nr) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                s[4 * c] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                s[4 * c + 1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                s[4 * c + 2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                s[4 * c + 3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        } else {
            memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

/* Key schedule for a 16- or 32-byte key. */
static int aes_setup(uint8_t rk[240], uint8_t *nr, const uint8_t *key, size_t klen) {
    if (klen == 16) *nr = 10;
    else if (klen == 32) *nr = 14;
    else return NEMA_ERR_INVAL;
    aes_expand(rk, key, (int)klen / 4, *nr);
    return NEMA_OK;
}

/* ---- GHASH ---- */

/* z = x * y in GF(2^128) (SP 800-38D algorithm 1). z may alias x or y. */
static void gf_mul(uint8_t z[16], const uint8_t x[16], const uint8_t y[16]) {
    uint8_t v[16], r[16];
    memcpy(v, y, 16);
    memset(r, 0, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1)
            for (int j = 0; j < 16; j++) r[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(z, r, 16);
}

static void ghash_absorb(uint8_t y[16], const uint8_t h[16], const uint8_t *p, size_t n) {
    while (n > 0) {
        uint8_t blk[16] = {0};
        size_t take = n < 16 ? n : 16;
        memcpy(blk, p, take);
        for (int i = 0; i < 16; i++) y[i] ^= blk[i];
        gf_mul(y, y, h);
        p += take, n -= take;
    }
}

static void ghash(uint8_t out[16], const uint8_t h[16], const uint8_t *aad, size_t alen, const uint8_t *ct, size_t clen) {
    uint8_t y[16] = {0}, lens[16];
    ghash_absorb(y, h, aad, alen);
    ghash_absorb(y, h, ct, clen);
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(((uint64_t)alen * 8) >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(((uint64_t)clen * 8) >> (56 - 8 * i));
    }
    for (int i = 0; i < 16; i++) y[i] ^= lens[i];
    gf_mul(y, y, h);
    memcpy(out, y, 16);
}

static void inc32(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--)
        if (++ctr[i] != 0) break;
}

/* CTR over the buffer with counter blocks starting at ctr (inc32 per block). */
static void gcm_crypt(const soft_gcm_t *g, uint8_t ctr[16], uint8_t *buf, size_t len) {
    uint8_t ks[16];
    while (len > 0) {
        aes_encrypt(g->rk, g->nr, ctr, ks);
        size_t take = len < 16 ? len : 16;
        for (size_t i = 0; i < take; i++) buf[i] ^= ks[i];
        buf += take, len -= take;
        inc32(ctr);
    }
}

/* ---- the interface ---- */

int ci_gcm_init(ci_cipher_t *c, const uint8_t *key, size_t klen) {
    soft_cipher_t *s = (soft_cipher_t *)c;
    memset(s, 0, sizeof(*s));
    if (!key) return NEMA_ERR_INVAL;
    int rc = aes_setup(s->u.gcm.rk, &s->u.gcm.nr, key, klen);
    if (rc != NEMA_OK) return rc;
    uint8_t zero[16] = {0};
    aes_encrypt(s->u.gcm.rk, s->u.gcm.nr, zero, s->u.gcm.h);
    s->kind = KIND_GCM;
    return NEMA_OK;
}

static int gcm_j0(const uint8_t nonce[12], uint8_t j0[16]) {
    if (!nonce) return NEMA_ERR_INVAL;
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    return NEMA_OK;
}

int ci_gcm_seal(ci_cipher_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf, size_t len,
                uint8_t tag[16]) {
    soft_cipher_t *s = (soft_cipher_t *)c;
    if (s->kind != KIND_GCM) return NEMA_ERR_STATE;
    uint8_t j0[16], ctr[16], ek[16], sum[16];
    if (gcm_j0(nonce, j0) != NEMA_OK || (!buf && len) || (!aad && alen)) return NEMA_ERR_INVAL;
    aes_encrypt(s->u.gcm.rk, s->u.gcm.nr, j0, ek);
    memcpy(ctr, j0, 16);
    inc32(ctr);
    gcm_crypt(&s->u.gcm, ctr, buf, len);
    ghash(sum, s->u.gcm.h, aad, alen, buf, len);
    for (int i = 0; i < 16; i++) tag[i] = sum[i] ^ ek[i];
    return NEMA_OK;
}

int ci_gcm_open(ci_cipher_t *c, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf, size_t len,
                const uint8_t tag[16]) {
    soft_cipher_t *s = (soft_cipher_t *)c;
    if (s->kind != KIND_GCM) return NEMA_ERR_STATE;
    uint8_t j0[16], ctr[16], ek[16], sum[16];
    if (gcm_j0(nonce, j0) != NEMA_OK || (!buf && len) || (!aad && alen)) return NEMA_ERR_INVAL;
    aes_encrypt(s->u.gcm.rk, s->u.gcm.nr, j0, ek);
    ghash(sum, s->u.gcm.h, aad, alen, buf, len); /* over the ciphertext, before anything is decrypted */
    for (int i = 0; i < 16; i++) sum[i] ^= ek[i];
    if (crypto_verify16(sum, tag) != 0) return NEMA_ERR_AUTH; /* buffer untouched: still ciphertext */
    memcpy(ctr, j0, 16);
    inc32(ctr);
    gcm_crypt(&s->u.gcm, ctr, buf, len);
    return NEMA_OK;
}

int ci_ctr_init(ci_cipher_t *c, const uint8_t *key, size_t klen, const uint8_t iv[16]) {
    soft_cipher_t *s = (soft_cipher_t *)c;
    memset(s, 0, sizeof(*s));
    if (!key || !iv) return NEMA_ERR_INVAL;
    int rc = aes_setup(s->u.ctr.rk, &s->u.ctr.nr, key, klen);
    if (rc != NEMA_OK) return rc;
    memcpy(s->u.ctr.ctr, iv, 16);
    s->u.ctr.used = 16;
    s->kind = KIND_CTR;
    return NEMA_OK;
}

/* The whole counter block is incremented (big-endian), as in SP 800-38A. */
static void ctr_inc128(uint8_t ctr[16]) {
    for (int i = 15; i >= 0; i--)
        if (++ctr[i] != 0) break;
}

void ci_ctr_xor(ci_cipher_t *c, uint8_t *buf, size_t len) {
    soft_cipher_t *s = (soft_cipher_t *)c;
    if (s->kind != KIND_CTR) return;
    soft_ctr_t *k = &s->u.ctr;
    for (size_t i = 0; i < len; i++) {
        if (k->used == 16) {
            aes_encrypt(k->rk, k->nr, k->ctr, k->ks);
            ctr_inc128(k->ctr);
            k->used = 0;
        }
        buf[i] ^= k->ks[k->used++];
    }
}

#endif /* !ESP_PLATFORM */
