/* Host hash primitives (cryptoimpl.h): SHA-1, SHA-256 (system/src/sha256.c), SHA-384 and SHA-512 (FIPS 180-4). Portable
 * C, the reference the host tests check against. Compiled only off ESP; ESP uses cryptoimpl_mbedtls.c. The 80 SHA-512
 * round constants and the SHA-384/512 initial values are the FIPS 180-4 definitions, computed from the cube and square
 * roots of the primes (tables in cryptoimpl_soft_tables.inc). */
#ifndef ESP_PLATFORM

#include <string.h>

#include "cryptoimpl.h"
#include "nema/types.h"
#include "pal/sha256.h"

typedef struct {
    uint32_t h[5];
    uint64_t len; /* bytes */
    uint8_t buf[64];
    size_t n;
} sha1_t;

typedef struct {
    uint64_t h[8];
    uint64_t len; /* bytes */
    uint8_t buf[128];
    size_t n;
} sha512_t;

typedef struct {
    uint8_t alg;
    union {
        pal_sha256_t s256;
        sha1_t s1;
        sha512_t s512;
    } u;
} soft_hash_t;

_Static_assert(sizeof(soft_hash_t) <= CI_HASH_STATE_MAX, "soft hash state does not fit cryptoimpl.h");

#include "cryptoimpl_soft_tables.inc"

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

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

/* ---- SHA-1 ---- */

static void sha1_block(sha1_t *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & cc) | (~b & d), k = 0x5a827999u;
        else if (i < 40) f = b ^ cc ^ d, k = 0x6ed9eba1u;
        else if (i < 60) f = (b & cc) | (b & d) | (cc & d), k = 0x8f1bbcdcu;
        else f = b ^ cc ^ d, k = 0xca62c1d6u;
        uint32_t t = ROL32(a, 5) + f + e + k + w[i];
        e = d, d = cc, cc = ROL32(b, 30), b = a, a = t;
    }
    c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e;
}

static void sha1_init(sha1_t *c) {
    static const uint32_t iv[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
}

static void sha1_update(sha1_t *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take, p += take, n -= take;
        if (c->n == 64) {
            sha1_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha1_final(sha1_t *c, uint8_t *out) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0;
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(c->h[i] >> (24 - 8 * j));
}

/* ---- SHA-384 / SHA-512 ---- */

static void sha512_block(sha512_t *c, const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[8 * i + j];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + maj;
        h = g, g = f, f = e, e = d + t1, d = cc, cc = b, b = a, a = t1 + t2;
    }
    c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e, c->h[5] += f, c->h[6] += g, c->h[7] += h;
}

static void sha512_init(sha512_t *c, const uint64_t iv[8]) {
    memcpy(c->h, iv, 8 * sizeof(uint64_t));
    c->len = 0;
    c->n = 0;
}

static void sha512_update(sha512_t *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n > 0) {
        size_t take = 128 - c->n < n ? 128 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take, p += take, n -= take;
        if (c->n == 128) {
            sha512_block(c, c->buf);
            c->n = 0;
        }
    }
}

/* Pads to 112 mod 128, then the 128-bit length in bits (the high 64 bits are zero for any length we can hold). */
static void sha512_final(sha512_t *c, uint8_t *out, size_t outlen) {
    uint64_t bits = c->len * 8;
    uint8_t pad[128];
    size_t padlen = (c->n < 112) ? 112 - c->n : 240 - c->n;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    sha512_update(c, pad, padlen);
    uint8_t lenb[16];
    memset(lenb, 0, 8);
    for (int i = 0; i < 8; i++) lenb[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_update(c, lenb, 16);
    uint8_t full[64];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) full[8 * i + j] = (uint8_t)(c->h[i] >> (56 - 8 * j));
    memcpy(out, full, outlen);
    memset(full, 0, sizeof(full));
}

/* ---- the interface ---- */

int ci_hash_init(ci_hash_t *h, int alg) {
    soft_hash_t *s = (soft_hash_t *)h;
    memset(s, 0, sizeof(*s));
    switch (alg) {
    case CI_HASH_SHA1: sha1_init(&s->u.s1); break;
    case CI_HASH_SHA256: pal_sha256_init(&s->u.s256); break;
    case CI_HASH_SHA384: sha512_init(&s->u.s512, IV384); break;
    case CI_HASH_SHA512: sha512_init(&s->u.s512, IV512); break;
    default: return NEMA_ERR_UNSUPPORTED;
    }
    s->alg = (uint8_t)alg;
    return 0;
}

void ci_hash_update(ci_hash_t *h, const void *d, size_t n) {
    soft_hash_t *s = (soft_hash_t *)h;
    switch (s->alg) {
    case CI_HASH_SHA1: sha1_update(&s->u.s1, d, n); break;
    case CI_HASH_SHA256: pal_sha256_update(&s->u.s256, d, n); break;
    case CI_HASH_SHA384:
    case CI_HASH_SHA512: sha512_update(&s->u.s512, d, n); break;
    default: break;
    }
}

void ci_hash_final(ci_hash_t *h, uint8_t *out) {
    soft_hash_t *s = (soft_hash_t *)h;
    switch (s->alg) {
    case CI_HASH_SHA1: sha1_final(&s->u.s1, out); break;
    case CI_HASH_SHA256: {
        uint8_t full[32];
        pal_sha256_final(&s->u.s256, full);
        memcpy(out, full, 32);
        break;
    }
    case CI_HASH_SHA384: sha512_final(&s->u.s512, out, 48); break;
    case CI_HASH_SHA512: sha512_final(&s->u.s512, out, 64); break;
    default: break;
    }
    memset(s, 0, sizeof(*s));
}

#endif /* !ESP_PLATFORM */
