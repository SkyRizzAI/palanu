/* The .papp container, both formats, and the installer: parsing and its refusals, the signature and the policy
 * matrix, and install / scan / remove against a RAM filesystem. The registry, launcher and admission are in
 * test_papp_registry.c. Packages are built here by hand; the deflate fixture comes from tests/unit/tools. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "papp_fixtures.h"
#include "pal/papp.h"
#include "pal/registry.h"
#include "pal/sha256.h"
#include "pal/trust.h"
#include "../../system/src/crypto_prim.h"
#include "test.h"

#define KB 1024u
static const uint8_t MAGIC8[8] = {0, 'a', 's', 'm', 1, 0, 0, 0};
static const uint8_t WASM[] = {0, 'a', 's', 'm', 1, 0, 0, 0};

/* The test module: a WebAssembly header and a pattern, the same rule as the fixture generator. */
static void payload_fill(uint8_t *p, size_t n) {
    memcpy(p, MAGIC8, 8);
    for (size_t i = 8; i < n; i++) p[i] = (uint8_t)((i - 8) % 37);
}

static const uint8_t SEED_OK[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
static const uint8_t SEED_OTHER[32] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
                                       0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f};

static void pub_of(const uint8_t seed[32], uint8_t pk[32]) {
    uint8_t sk[64];
    prim_ed25519_keypair(sk, pk, seed);
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    for (int b = 0; b < 4; b++) p[b] = (uint8_t)(v >> (8 * b));
}

/* ---- a manifest builder: a CBOR map of text keys ---- */
typedef struct {
    uint8_t b[400];
    size_t n;
    int pairs;
} man_t;

static void mput(man_t *m, const void *p, size_t n) {
    memcpy(m->b + m->n, p, n);
    m->n += n;
}
static void mhead(man_t *m, int major, uint32_t n) {
    uint8_t h[5];
    size_t k = 0;
    if (n < 24) h[k++] = (uint8_t)(major << 5 | n);
    else if (n < 256) {
        h[k++] = (uint8_t)(major << 5 | 24);
        h[k++] = (uint8_t)n;
    } else {
        h[k++] = (uint8_t)(major << 5 | 25);
        h[k++] = (uint8_t)(n >> 8);
        h[k++] = (uint8_t)n;
    }
    mput(m, h, k);
}
static void mkey(man_t *m, const char *k) {
    mhead(m, 3, (uint32_t)strlen(k));
    mput(m, k, strlen(k));
}
static void mstr(man_t *m, const char *k, const char *v) {
    mkey(m, k);
    mhead(m, 3, (uint32_t)strlen(v));
    mput(m, v, strlen(v));
    m->pairs++;
}
static void mint(man_t *m, const char *k, uint32_t v) {
    mkey(m, k);
    mhead(m, 0, v);
    m->pairs++;
}
static void mbool(man_t *m, const char *k, bool v) {
    mkey(m, k);
    uint8_t b = v ? 0xF5 : 0xF4;
    mput(m, &b, 1);
    m->pairs++;
}
static void mcaps(man_t *m, const char *k, const char *const *c, int n) {
    mkey(m, k);
    mhead(m, 4, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        mhead(m, 3, (uint32_t)strlen(c[i]));
        mput(m, c[i], strlen(c[i]));
    }
    m->pairs++;
}
/* id, name, ver, api, kind (optional), caps (optional), hidden (optional) */
static size_t manifest(uint8_t *out, const char *id, uint32_t api, const char *kind, const char *const *caps, int ncaps, bool hidden) {
    man_t m;
    memset(&m, 0, sizeof(m));
    mstr(&m, "id", id);
    mstr(&m, "name", "Demo App");
    mstr(&m, "ver", "1.2.0");
    mint(&m, "api", api);
    if (kind) mstr(&m, "kind", kind);
    if (ncaps) mcaps(&m, "caps", caps, ncaps);
    if (hidden) mbool(&m, "hidden", true);
    out[0] = (uint8_t)(0xA0 | m.pairs);
    memcpy(out + 1, m.b, m.n);
    return m.n + 1;
}

/* A raw DEFLATE stored block (RFC 1951, BTYPE 00) holding d: a valid stream without any Huffman coding. */
static size_t stored_block(uint8_t *out, const uint8_t *d, size_t n) {
    out[0] = 0x01;
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)~out[1];
    out[4] = (uint8_t)~out[2];
    memcpy(out + 5, d, n);
    return n + 5;
}

/* ---- format 2 builder: header, section table, sections, SHA-256, and "SIG0" + signature when seed is given ---- */
typedef struct {
    uint8_t type, flags;
    const uint8_t *data;
    uint32_t stored, raw;
} sec_in_t;

static size_t build2(uint8_t *out, const sec_in_t *s, int n, const uint8_t *seed) {
    uint32_t hlen = 10u + 14u * (uint32_t)n, off = hlen;
    memcpy(out, PAL_PAPP_MAGIC, 4);
    out[4] = 2;
    out[5] = seed ? 2 : 0;
    put16(out + 6, hlen);
    put16(out + 8, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t *e = out + 10 + 14 * i;
        e[0] = s[i].type;
        e[1] = s[i].flags;
        put32(e + 2, off);
        put32(e + 6, s[i].stored);
        put32(e + 10, s[i].raw);
        memcpy(out + off, s[i].data, s[i].stored);
        off += s[i].stored;
    }
    pal_sha256_t h;
    pal_sha256_init(&h);
    pal_sha256_update(&h, out, off);
    pal_sha256_final(&h, out + off);
    off += 32;
    if (seed) {
        uint8_t sk[64], pk[32];
        prim_ed25519_keypair(sk, pk, seed);
        memcpy(out + off, "SIG0", 4);
        prim_ed25519_sign(out + off + 4, sk, out + off - 32, 32);
        off += 68;
    }
    return off;
}

/* The common package: manifest (stored), code (stored), optional icon. */
static size_t plain_pkg(uint8_t *out, const char *id, const char *kind, const char *const *caps, int ncaps, const uint8_t *seed) {
    uint8_t man[200], code[64];
    size_t ml = manifest(man, id, 2, kind, caps, ncaps, false);
    memcpy(code, WASM, sizeof(WASM));
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 0, code, sizeof(WASM), sizeof(WASM)}};
    return build2(out, s, 2, seed);
}

/* ---- the trust store: /etc/trust holds the trusted public key as hex text; /data/settings holds dev.mode ---- */
static void write_file(const char *path, const void *data, size_t n) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) return;
    nema_obj_write(f, data, n, 2000);
    nema_obj_unref(f);
}

static void set_dev(bool on) {
    char s[32];
    int n = snprintf(s, sizeof(s), "dev.mode=%d\n", on ? 1 : 0);
    write_file("/data/settings", s, (size_t)n);
}

static void trust_ok_key(void) {
    uint8_t pk[32];
    pub_of(SEED_OK, pk);
    char hex[66];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", pk[i]);
    hex[64] = '\n';
    write_file("/etc/trust/ok.pub", hex, 65);
}

static bool exists(const char *p) {
    nema_stat_t st;
    return nema_fs_stat(p, &st) == NEMA_OK;
}

/* ---- format 1 (unchanged from the first release) ---- */

/* Build a package by hand: header + CBOR manifest + payload + SHA-256. */
static size_t build(uint8_t *out, const uint8_t *manifest, size_t mlen, const uint8_t *payload, size_t plen) {
    memcpy(out, "PLNU", 4);
    out[4] = 1, out[5] = 0, out[6] = 20, out[7] = 0;
    uint32_t v[3] = {(uint32_t)mlen, (uint32_t)plen, (uint32_t)plen};
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 4; b++) out[8 + 4 * i + b] = (uint8_t)(v[i] >> (8 * b));
    memcpy(out + 20, manifest, mlen);
    memcpy(out + 20 + mlen, payload, plen);
    pal_sha256_t h;
    pal_sha256_init(&h);
    pal_sha256_update(&h, out, 20 + mlen + plen);
    pal_sha256_final(&h, out + 20 + mlen + plen);
    return 20 + mlen + plen + 32;
}

/* {"id":"demo","api":1,"perm":["fs"],"mem":128} */
static const uint8_t M_OK[] = {0xA4, 0x62, 'i', 'd', 0x64, 'd', 'e', 'm', 'o', 0x63, 'a', 'p', 'i', 0x01, 0x64, 'p', 'e', 'r', 'm',
                               0x81, 0x62, 'f', 's', 0x63, 'm', 'e', 'm', 0x18, 0x80};

/* {"id":"demo","api":2,"perm":["fs","net.listen"]}: at level 2 "fs" is /data and /tmp only */
static const uint8_t M_V2[] = {0xA3, 0x62, 'i', 'd', 0x64, 'd', 'e', 'm', 'o', 0x63, 'a', 'p', 'i', 0x02, 0x64, 'p', 'e', 'r',
                               'm', 0x82, 0x62, 'f', 's', 0x6A, 'n', 'e', 't', '.', 'l', 'i', 's', 't', 'e', 'n'};

TEST(parse_level2_permissions_are_exact) {
    uint8_t f[256];
    size_t n = build(f, M_V2, sizeof(M_V2), WASM, sizeof(WASM));
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(f, n, &a), NEMA_OK);
    CHECK_EQ(a.api, 2);
    CHECK(a.perms == (PAL_PERM_FS | PAL_PERM_NET_LISTEN));
    CHECK_EQ(a.format, PAL_PAPP_FORMAT_V1);
}

/* {"id":"demo","api":2,"perm":["proc.spawn","proc"]}: both names grant the proc bit (api level 2, child processes) */
static const uint8_t M_PROC[] = {0xA3, 0x62, 'i', 'd', 0x64, 'd', 'e', 'm', 'o', 0x63, 'a', 'p', 'i', 0x02, 0x64, 'p', 'e', 'r',
                                 'm', 0x82, 0x6A, 'p', 'r', 'o', 'c', '.', 's', 'p', 'a', 'w', 'n', 0x64, 'p', 'r', 'o', 'c'};

TEST(parse_accepts_the_proc_spawn_permission) {
    uint8_t f[256];
    size_t n = build(f, M_PROC, sizeof(M_PROC), WASM, sizeof(WASM));
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(f, n, &a), NEMA_OK);
    CHECK(a.perms == PAL_PERM_PROC);
}

TEST(sha256_known_vectors) {
    uint8_t d[32];
    pal_sha256_t c;
    pal_sha256_init(&c);
    pal_sha256_update(&c, "abc", 3);
    pal_sha256_final(&c, d);
    static const uint8_t abc[4] = {0xba, 0x78, 0x16, 0xbf};
    CHECK(memcmp(d, abc, 4) == 0 && d[31] == 0xad);
    pal_sha256_init(&c); /* the two-block message from FIPS 180-4, fed in odd pieces */
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    pal_sha256_update(&c, m, 5);
    pal_sha256_update(&c, m + 5, strlen(m) - 5);
    pal_sha256_final(&c, d);
    CHECK(d[0] == 0x24 && d[1] == 0x8d && d[2] == 0x6a && d[3] == 0x61 && d[31] == 0xc1);
}

TEST(parse_accepts_a_good_package) {
    uint8_t f[256];
    size_t n = build(f, M_OK, sizeof(M_OK), WASM, sizeof(WASM));
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(f, n, &a), NEMA_OK);
    CHECK(strcmp(a.id, "demo") == 0);
    CHECK_EQ(a.api, 1);
    /* API level 1: "fs" predates the namespace, so it also grants /home (fs.home) */
    CHECK(a.perms == (PAL_PERM_FS | PAL_PERM_FS_HOME));
    CHECK_EQ(a.mem_kb, 128);
    CHECK_EQ(a.payload_len, sizeof(WASM));
    CHECK(memcmp(a.payload, WASM, sizeof(WASM)) == 0);
    CHECK(a.kind == PAL_KIND_TTY); /* no kind in a format 1 manifest: a command, as before */
    CHECK(a.owned == NULL && a.sig == NULL);
}

TEST(parse_rejects_any_changed_byte) {
    uint8_t f[256];
    size_t n = build(f, M_OK, sizeof(M_OK), WASM, sizeof(WASM));
    pal_papp_t a;
    for (size_t i = 0; i < n; i++) {
        uint8_t keep = f[i];
        f[i] ^= 0x01;
        CHECK(pal_papp_parse(f, n, &a) != NEMA_OK); /* header, manifest, payload and hash are all covered */
        f[i] = keep;
    }
    CHECK_EQ(pal_papp_parse(f, n - 1, &a) == NEMA_OK, 0);
}

TEST(parse_rejects_bad_manifests) {
    uint8_t f[256];
    pal_papp_t a;
    /* unknown permission is refused, not ignored: {"id":"x","perm":["root"]} */
    static const uint8_t M_PERM[] = {0xA2, 0x62, 'i', 'd', 0x61, 'x', 0x64, 'p', 'e', 'r', 'm', 0x81, 0x64, 'r', 'o', 'o', 't'};
    size_t n = build(f, M_PERM, sizeof(M_PERM), WASM, sizeof(WASM));
    CHECK_EQ(pal_papp_parse(f, n, &a), NEMA_ERR_UNSUPPORTED);
    /* an id with a slash would escape /apps: {"id":"../x","api":1} */
    static const uint8_t M_ID[] = {0xA2, 0x62, 'i', 'd', 0x65, '.', '.', '/', 'x', 'y', 0x63, 'a', 'p', 'i', 0x01};
    n = build(f, M_ID, sizeof(M_ID), WASM, sizeof(WASM));
    CHECK_EQ(pal_papp_parse(f, n, &a), NEMA_ERR_INVAL);
    /* an app that needs a newer API than this firmware: {"id":"x","api":9} */
    static const uint8_t M_API[] = {0xA2, 0x62, 'i', 'd', 0x61, 'x', 0x63, 'a', 'p', 'i', 0x09};
    n = build(f, M_API, sizeof(M_API), WASM, sizeof(WASM));
    CHECK_EQ(pal_papp_parse(f, n, &a), NEMA_ERR_UNSUPPORTED);
    /* not a package at all */
    memset(f, 0, 64);
    CHECK(pal_papp_parse(f, 64, &a) != NEMA_OK);
}

/* ---- format 2 ---- */

TEST(v2_plain_sections_parse_with_kind_caps_and_icon) {
    static const char *const caps[] = {"fs", "net"};
    uint8_t pkg[1024], man[200], code[64], icon[12];
    memset(icon, 7, sizeof(icon));
    size_t ml = manifest(man, "demo", 2, "gui", caps, 2, true);
    memcpy(code, WASM, sizeof(WASM));
    sec_in_t s[3] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml},
                     {2, 0, code, sizeof(WASM), sizeof(WASM)},
                     {3, 0, icon, sizeof(icon), sizeof(icon)}};
    size_t n = build2(pkg, s, 3, NULL);
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_OK);
    CHECK_EQ(a.format, PAL_PAPP_FORMAT);
    CHECK(a.kind == PAL_KIND_GUI);
    CHECK(a.hidden);
    CHECK(a.perms == (PAL_PERM_FS | PAL_PERM_NET));
    CHECK_EQ(a.payload_len, sizeof(WASM));
    CHECK(memcmp(a.payload, WASM, sizeof(WASM)) == 0);
    CHECK(a.owned == NULL); /* stored code is used in place */
    CHECK(a.sig == NULL);
}

TEST(v2_deflated_sections_inflate_into_owned_memory) {
    uint8_t pkg[2048], man[200], mdef[300];
    size_t ml = manifest(man, "zipped", 2, "service", NULL, 0, false);
    size_t mdl = stored_block(mdef, man, ml); /* the manifest is deflated too (a stored block) */
    sec_in_t s[2] = {{1, 1, mdef, (uint32_t)mdl, (uint32_t)ml}, {2, 1, FX_DEFLATE, FX_DEFLATE_LEN, FX_PAYLOAD_LEN}};
    size_t n = build2(pkg, s, 2, NULL);
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_OK);
    CHECK(a.kind == PAL_KIND_SERVICE && a.background);
    CHECK(a.owned != NULL);
    CHECK_EQ(a.payload_len, FX_PAYLOAD_LEN);
    uint8_t want[FX_PAYLOAD_LEN];
    payload_fill(want, sizeof(want));
    CHECK(memcmp(a.payload, want, sizeof(want)) == 0); /* dynamic Huffman, from zlib */
    pal_papp_release(&a);
    CHECK(a.owned == NULL);
}

TEST(v2_stored_deflate_block_inflates) {
    uint8_t pkg[512], code[64], cdef[80];
    memcpy(code, WASM, sizeof(WASM));
    size_t cl = stored_block(cdef, code, sizeof(WASM));
    static const char *const caps[] = {"crypto.keys"};
    uint8_t man[200];
    size_t ml = manifest(man, "stored", 2, "tty", caps, 1, false);
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 1, cdef, (uint32_t)cl, sizeof(WASM)}};
    size_t n = build2(pkg, s, 2, NULL);
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_OK);
    CHECK(memcmp(a.payload, WASM, sizeof(WASM)) == 0);
    CHECK(a.perms == PAL_PERM_CRYPTO_KEYS);
    pal_papp_release(&a);
}

TEST(v2_inflated_size_must_match_the_header) {
    uint8_t pkg[2048], man[200];
    size_t ml = manifest(man, "short", 2, "tty", NULL, 0, false);
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 1, FX_DEFLATE, FX_DEFLATE_LEN, FX_PAYLOAD_LEN - 1}};
    size_t n = build2(pkg, s, 2, NULL);
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_PROTO);
    CHECK(a.owned == NULL);
}

TEST(v2_bad_hash_is_refused) {
    uint8_t pkg[1024], man[200], code[64];
    size_t ml = manifest(man, "hashy", 2, "tty", NULL, 0, false);
    memcpy(code, WASM, sizeof(WASM));
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 0, code, sizeof(WASM), sizeof(WASM)}};
    size_t n = build2(pkg, s, 2, NULL);
    pal_papp_t a;
    pkg[n - 1] ^= 0x80; /* the hash itself */
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_PERM);
    pkg[n - 1] ^= 0x80;
    pkg[n - 40] ^= 0x01; /* a code byte */
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_PERM);
}

TEST(v2_truncation_at_every_length_is_refused) {
    uint8_t pkg[2048], man[200];
    size_t ml = manifest(man, "cut", 2, "tty", NULL, 0, false);
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 1, FX_DEFLATE, FX_DEFLATE_LEN, FX_PAYLOAD_LEN}};
    size_t n = build2(pkg, s, 2, SEED_OK);
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_OK);
    pal_papp_release(&a);
    int refused = 0;
    for (size_t len = 0; len < n; len++) {
        if (pal_papp_parse(pkg, len, &a) != NEMA_OK) refused++;
        pal_papp_release(&a);
    }
    CHECK_EQ(refused, (int)n); /* every proper prefix is refused, none is read past its end */
}

TEST(v2_unknown_and_aot_sections_are_refused) {
    uint8_t pkg[1024], man[200], code[64];
    size_t ml = manifest(man, "typed", 2, "tty", NULL, 0, false);
    memcpy(code, WASM, sizeof(WASM));
    pal_papp_t a;
    sec_in_t unknown[3] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 0, code, sizeof(WASM), sizeof(WASM)}, {9, 0, code, 8, 8}};
    size_t n = build2(pkg, unknown, 3, NULL);
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_UNSUPPORTED);
    sec_in_t aot[3] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 0, code, sizeof(WASM), sizeof(WASM)}, {5, 0, code, 8, 8}};
    n = build2(pkg, aot, 3, NULL);
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_UNSUPPORTED); /* AOT is reserved: rejected, not ignored */
    sec_in_t nocode[1] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}};
    n = build2(pkg, nocode, 1, NULL);
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_PROTO); /* nsect below 2 */
}

TEST(v2_oversize_code_is_refused_before_inflating) {
    uint8_t pkg[2048], man[200];
    size_t ml = manifest(man, "huge", 2, "tty", NULL, 0, false);
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 1, FX_DEFLATE, FX_DEFLATE_LEN, PAL_PAPP_MAX_BYTES + 1}};
    size_t n = build2(pkg, s, 2, NULL);
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_RANGE);
    CHECK(a.owned == NULL);
}

TEST(v2_signature_is_verified_against_the_trusted_keys) {
    static const char *const caps[] = {"net"};
    uint8_t pkg[1024], man[200], code[64];
    size_t ml = manifest(man, "signed", 2, "tty", caps, 1, false);
    memcpy(code, WASM, sizeof(WASM));
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 0, code, sizeof(WASM), sizeof(WASM)}};
    size_t n = build2(pkg, s, 2, SEED_OK);
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_OK);
    CHECK(a.sig != NULL);
    uint8_t ok[32], other[32];
    pub_of(SEED_OK, ok);
    pub_of(SEED_OTHER, other);
    const uint8_t(*good)[32] = (const uint8_t(*)[32])ok;
    const uint8_t(*bad)[32] = (const uint8_t(*)[32])other;
    CHECK(psys_trust_verify(a.digest, a.sig, good, 1) == PAL_SIG_TRUSTED);
    CHECK(psys_trust_verify(a.digest, a.sig, bad, 1) == PAL_SIG_UNTRUSTED);
    CHECK(psys_trust_verify(a.digest, a.sig, good, 0) == PAL_SIG_UNTRUSTED); /* no trusted key at all */
    CHECK(psys_trust_verify(a.digest, NULL, good, 1) == PAL_SIG_NONE);
    /* a signature with one bit changed is not a signature by the key */
    uint8_t forged[64];
    memcpy(forged, a.sig, 64);
    forged[10] ^= 0x04;
    CHECK(psys_trust_verify(a.digest, forged, good, 1) == PAL_SIG_UNTRUSTED);
}

/* A package written by the Forge packer (papp_fixtures.h, from tests/unit/tools): the device reads what Forge writes. */
TEST(forge_packed_signed_package_parses_and_verifies) {
    pal_papp_t a;
    CHECK_EQ(pal_papp_parse(FX_TS_SIGNED, FX_TS_LEN, &a), NEMA_OK);
    CHECK(strcmp(a.id, "tsdemo") == 0);
    CHECK(a.kind == PAL_KIND_GUI && a.hidden);
    CHECK(strcmp(a.icon, "tsicon") == 0);
    CHECK(a.perms == PAL_PERM_NET);
    CHECK(a.sig != NULL);
    uint8_t ok[32];
    pub_of(SEED_OK, ok);
    const uint8_t(*keys)[32] = (const uint8_t(*)[32])ok;
    CHECK(psys_trust_verify(a.digest, a.sig, keys, 1) == PAL_SIG_TRUSTED);
    pal_papp_release(&a);
}

TEST(v2_changes_after_signing_are_refused) {
    uint8_t pkg[1024], man[200], code[64];
    size_t ml = manifest(man, "later", 2, "tty", NULL, 0, false);
    memcpy(code, WASM, sizeof(WASM));
    sec_in_t s[2] = {{1, 0, man, (uint32_t)ml, (uint32_t)ml}, {2, 0, code, sizeof(WASM), sizeof(WASM)}};
    size_t n = build2(pkg, s, 2, SEED_OK);
    pal_papp_t a;
    pkg[n - 68 - 40] ^= 0x01; /* a code byte: the signature no longer covers the hash */
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_ERR_PERM);
    pkg[n - 68 - 40] ^= 0x01;
    pkg[n - 2] ^= 0x01; /* the signature only: parse still finds the hash right, and the signature check says no */
    CHECK_EQ(pal_papp_parse(pkg, n, &a), NEMA_OK);
    pal_papp_release(&a);
    pkg[n - 68 + 0] ^= 0x01; /* "SIG0" itself */
    CHECK(pal_papp_parse(pkg, n, &a) != NEMA_OK);
    pkg[n - 68 + 0] ^= 0x01;
    pkg[5] = 0; /* the flag says unsigned but the trailer is there: the hash is found in the wrong place */
    CHECK(pal_papp_parse(pkg, n, &a) != NEMA_OK);
}

/* ---- the policy matrix (trust.c): developer mode x signature x sensitive capabilities ---- */
TEST(policy_matrix) {
    struct {
        pal_sig_t sig;
        bool dev;
        uint32_t caps;
        nema_err_t want;
    } M[] = {
        {PAL_SIG_TRUSTED, false, 0, NEMA_OK},
        {PAL_SIG_TRUSTED, false, PAL_PERM_NET | PAL_PERM_CRYPTO_KEYS, NEMA_OK},
        {PAL_SIG_TRUSTED, true, 0, NEMA_OK},
        {PAL_SIG_TRUSTED, true, PAL_PERM_NET, NEMA_OK},
        {PAL_SIG_UNTRUSTED, false, 0, NEMA_ERR_PERM},
        {PAL_SIG_UNTRUSTED, false, PAL_PERM_NET, NEMA_ERR_PERM},
        {PAL_SIG_UNTRUSTED, true, 0, NEMA_OK},
        {PAL_SIG_UNTRUSTED, true, PAL_PERM_NET, NEMA_OK},
        {PAL_SIG_NONE, false, 0, NEMA_ERR_PERM},
        {PAL_SIG_NONE, false, PAL_PERM_CRYPTO_KEYS, NEMA_ERR_PERM},
        {PAL_SIG_NONE, true, 0, NEMA_OK},
        {PAL_SIG_NONE, true, PAL_PERM_CRYPTO_KEYS, NEMA_OK},
    };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
        char note[120];
        CHECK_EQ(psys_policy(M[i].sig, M[i].caps, M[i].dev, note, sizeof(note)), M[i].want);
        if (M[i].want == NEMA_OK && M[i].sig != PAL_SIG_TRUSTED) CHECK(strstr(note, "developer mode") != NULL);
        if (M[i].want != NEMA_OK) CHECK(strstr(note, "refused") != NULL);
    }
    char note[120];
    psys_policy(PAL_SIG_NONE, PAL_PERM_NET, false, note, sizeof(note));
    CHECK(strstr(note, "net") != NULL); /* the refusal names the sensitive capability */
}

TEST(dev_mode_is_read_from_the_settings_file) {
    set_dev(true);
    CHECK(psys_dev_mode());
    set_dev(false);
    CHECK(!psys_dev_mode());
    write_file("/data/settings", "theme=dark\ndev.mode=1\n", 23);
    CHECK(psys_dev_mode());
    set_dev(false); /* the install tests start in normal mode */
}

TEST(dev_mode_off_is_found_late_in_a_large_settings_file) {
    /* The line used to be looked for in the first 512 bytes only: with a PIN set and more keys before it, an owner's "off"
     * was not seen and the factory default came back. The file is at most 1536 bytes. */
    char text[1600];
    int n = 0;
    for (int i = 0; n < 1300; i++) n += snprintf(text + n, sizeof(text) - (size_t)n, "key.%03d=padding-value-%03d\n", i, i);
    n += snprintf(text + n, sizeof(text) - (size_t)n, "dev.mode=0\n");
    write_file("/data/settings", text, (size_t)n + 1);
    CHECK(!psys_dev_mode());
    n -= 12;
    n += snprintf(text + n, sizeof(text) - (size_t)n, "dev.mode=1\n");
    write_file("/data/settings", text, (size_t)n + 1);
    CHECK(psys_dev_mode());
    write_file("/data/settings", "theme=dark\n", 12); /* no line: the build's default, which a host build has on */
    CHECK_EQ(psys_dev_mode(), PSYS_DEV_MODE_DEFAULT != 0);
    set_dev(false);
}

/* ---- the installer, against a RAM filesystem: /apps, /etc/trust, /data, /tmp ---- */

static void fs_setup(void) {
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(2u * 1024u * 1024u));
    nema_fs_mkdir("/apps");
    nema_fs_mkdir("/etc");
    nema_fs_mkdir("/etc/trust");
    nema_fs_mkdir("/data");
    nema_fs_mkdir("/tmp");
    nema_fs_mkdir("/tmp/in");
    trust_ok_key();
    set_dev(false);
}

TEST(install_signed_package_registers_command_and_entry) {
    static const char *const caps[] = {"net"};
    uint8_t pkg[1024];
    size_t n = plain_pkg(pkg, "demo_signed", "tty", caps, 1, SEED_OK);
    write_file("/tmp/in/demo_signed.papp", pkg, n);
    char id[24] = "", msg[120] = "";
    CHECK_EQ(pal_papp_install_msg("/tmp/in/demo_signed.papp", id, sizeof(id), msg, sizeof(msg)), NEMA_OK);
    CHECK(strcmp(id, "demo_signed") == 0);
    CHECK(exists("/apps/demo_signed/app.papp"));
    CHECK(!exists("/apps/.staging/demo_signed.papp")); /* the staging copy was moved, not copied */
    CHECK(nema_app_find("demo_signed") != NULL);
    pal_reg_app_t e;
    CHECK(psys_reg_find("demo_signed", &e));
    CHECK(e.source == PAL_SRC_INSTALLED && e.caps == PAL_PERM_NET);
    CHECK(strcmp(e.path, "/apps/demo_signed/app.papp") == 0);
    /* the same name again is refused until it is removed */
    CHECK_EQ(pal_papp_install_msg("/tmp/in/demo_signed.papp", NULL, 0, msg, sizeof(msg)), NEMA_ERR_EXIST);
    CHECK(strstr(msg, "app remove demo_signed") != NULL);
}

TEST(install_unsigned_package_needs_developer_mode) {
    uint8_t pkg[1024];
    size_t n = plain_pkg(pkg, "demo_unsigned", "tty", NULL, 0, NULL);
    write_file("/tmp/in/demo_unsigned.papp", pkg, n);
    char msg[120] = "";
    CHECK_EQ(pal_papp_install_msg("/tmp/in/demo_unsigned.papp", NULL, 0, msg, sizeof(msg)), NEMA_ERR_PERM);
    CHECK(strstr(msg, "refused") != NULL);
    CHECK(!exists("/apps/.staging/demo_unsigned.papp")); /* a refused package leaves nothing behind */
    CHECK(!exists("/apps/demo_unsigned"));
    CHECK(nema_app_find("demo_unsigned") == NULL);
    set_dev(true);
    CHECK_EQ(pal_papp_install_msg("/tmp/in/demo_unsigned.papp", NULL, 0, msg, sizeof(msg)), NEMA_OK);
    CHECK(exists("/apps/demo_unsigned/app.papp"));
    CHECK_EQ(pal_papp_remove("demo_unsigned"), NEMA_OK);
    set_dev(false);
}

TEST(install_refuses_damaged_and_builtin_named_packages) {
    uint8_t pkg[1024];
    size_t n = plain_pkg(pkg, "demo_dmg", "tty", NULL, 0, SEED_OK);
    pkg[n - 80] ^= 0x10; /* a code byte */
    write_file("/tmp/in/demo_dmg.papp", pkg, n);
    char msg[120] = "";
    CHECK_EQ(pal_papp_install_msg("/tmp/in/demo_dmg.papp", NULL, 0, msg, sizeof(msg)), NEMA_ERR_PERM);
    CHECK(strstr(msg, "not a valid package") != NULL);
    CHECK(!exists("/apps/.staging/demo_dmg.papp"));
    CHECK(!exists("/apps/demo_dmg"));
    /* "terminal" is a view of the built-in table: a package cannot take its name */
    n = plain_pkg(pkg, "terminal", "tty", NULL, 0, SEED_OK);
    write_file("/tmp/in/terminal.papp", pkg, n);
    CHECK_EQ(pal_papp_install_msg("/tmp/in/terminal.papp", NULL, 0, msg, sizeof(msg)), NEMA_ERR_EXIST);
    CHECK(strstr(msg, "built-in") != NULL);
    CHECK(!exists("/apps/terminal"));
}

TEST(remove_deletes_files_entry_and_command) {
    CHECK(exists("/apps/demo_signed/app.papp"));
    /* what the app wrote while it ran: data/ (nested) and the kv log and its temporary */
    nema_fs_mkdir("/apps/demo_signed/data");
    nema_fs_mkdir("/apps/demo_signed/data/sub");
    write_file("/apps/demo_signed/data/sub/x.txt", "x", 1);
    write_file("/apps/demo_signed/kv.dat", "kv", 2);
    write_file("/apps/demo_signed/kv.tmp", "t", 1);
    CHECK_EQ(pal_papp_remove("demo_signed"), NEMA_OK);
    CHECK(!exists("/apps/demo_signed/app.papp"));
    CHECK(!exists("/apps/demo_signed/data/sub/x.txt"));
    CHECK(!exists("/apps/demo_signed/kv.dat"));
    CHECK(!exists("/apps/demo_signed/kv.tmp"));
    CHECK(!exists("/apps/demo_signed")); /* nothing under /apps/<id> is left */
    pal_reg_app_t e;
    CHECK(!psys_reg_find("demo_signed", &e));
    CHECK(nema_app_find("demo_signed") == NULL);
    CHECK_EQ(pal_papp_remove("demo_signed"), NEMA_ERR_NOENT);
    CHECK_EQ(pal_papp_remove("terminal"), NEMA_ERR_NOENT); /* built-ins are not removed */
}

TEST(scan_registers_valid_packages_and_ignores_staging) {
    uint8_t pkg[1024], bad[1024];
    size_t n = plain_pkg(pkg, "flat_ok", "tty", NULL, 0, SEED_OK);
    write_file("/apps/flat_ok.papp", pkg, n); /* the Link and cp layout: /apps/<id>.papp */
    n = plain_pkg(bad, "flat_bad", "tty", NULL, 0, NULL);
    write_file("/apps/flat_bad.papp", bad, n); /* unsigned, developer mode off */
    nema_fs_mkdir("/apps/.staging");
    n = plain_pkg(pkg, "ghost", "tty", NULL, 0, SEED_OK);
    write_file("/apps/.staging/ghost.papp", pkg, n);
    CHECK_EQ(pal_papp_scan(), 1);
    CHECK(nema_app_find("flat_ok") != NULL);
    CHECK(nema_app_find("flat_bad") == NULL);
    CHECK(nema_app_find("ghost") == NULL);
    CHECK_EQ(pal_papp_scan(), 0); /* already registered: nothing new */
    CHECK_EQ(pal_papp_remove("flat_ok"), NEMA_OK);
    CHECK(!exists("/apps/flat_ok.papp"));
}

TEST(v1_package_is_accepted_only_in_developer_mode) {
    uint8_t f[256];
    size_t n = build(f, M_OK, sizeof(M_OK), WASM, sizeof(WASM)); /* id "demo", unsigned, format 1 */
    write_file("/tmp/in/demo.papp", f, n);
    char msg[120] = "";
    CHECK_EQ(pal_papp_install_msg("/tmp/in/demo.papp", NULL, 0, msg, sizeof(msg)), NEMA_ERR_PERM);
    set_dev(true);
    CHECK_EQ(pal_papp_install_msg("/tmp/in/demo.papp", NULL, 0, msg, sizeof(msg)), NEMA_OK);
    pal_reg_app_t e;
    CHECK(psys_reg_find("demo", &e) && e.kind == PAL_KIND_TTY);
    CHECK_EQ(pal_papp_remove("demo"), NEMA_OK);
    set_dev(false);
}

TEST(run_file_refuses_an_unsigned_package_without_developer_mode) {
    uint8_t pkg[1024];
    size_t n = plain_pkg(pkg, "runme", "tty", NULL, 0, NULL);
    write_file("/tmp/in/runme.papp", pkg, n);
    CHECK_EQ(pal_papp_run_file("/tmp/in/runme.papp", 1, (char *[]){"runme", NULL}), 126);
    CHECK_EQ(pal_papp_run_file("/tmp/in/missing.papp", 1, (char *[]){"x", NULL}), 1);
}

static const pal_reg_builtin_t BUILTIN[] = {
    {"terminal", "Terminal", "1.0.0", 2, PAL_KIND_GUI, false, 0, 0, 0, 0, "terminal", "Tools"},
};
static const pal_reg_defaults_t DEFAULTS = {PAL_KIND_TTY, true, 2};

int main(void) {
    nema_config_t cfg = {0};
    nema_init(&cfg);
    fs_setup();
    psys_reg_init(BUILTIN, 1, &DEFAULTS);
    RUN(sha256_known_vectors);
    RUN(parse_accepts_a_good_package);
    RUN(parse_level2_permissions_are_exact);
    RUN(parse_accepts_the_proc_spawn_permission);
    RUN(parse_rejects_any_changed_byte);
    RUN(parse_rejects_bad_manifests);
    RUN(v2_plain_sections_parse_with_kind_caps_and_icon);
    RUN(v2_deflated_sections_inflate_into_owned_memory);
    RUN(v2_stored_deflate_block_inflates);
    RUN(v2_inflated_size_must_match_the_header);
    RUN(v2_bad_hash_is_refused);
    RUN(v2_truncation_at_every_length_is_refused);
    RUN(v2_unknown_and_aot_sections_are_refused);
    RUN(v2_oversize_code_is_refused_before_inflating);
    RUN(v2_signature_is_verified_against_the_trusted_keys);
    RUN(forge_packed_signed_package_parses_and_verifies);
    RUN(v2_changes_after_signing_are_refused);
    RUN(policy_matrix);
    RUN(dev_mode_is_read_from_the_settings_file);
    RUN(dev_mode_off_is_found_late_in_a_large_settings_file);
    RUN(install_signed_package_registers_command_and_entry);
    RUN(install_unsigned_package_needs_developer_mode);
    RUN(install_refuses_damaged_and_builtin_named_packages);
    RUN(remove_deletes_files_entry_and_command);
    RUN(scan_registers_valid_packages_and_ignores_staging);
    RUN(v1_package_is_accepted_only_in_developer_mode);
    RUN(run_file_refuses_an_unsigned_package_without_developer_mode);
    return TEST_DONE();
}
