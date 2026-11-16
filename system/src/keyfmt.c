/* Reads an OpenSSH private key file (PROTOCOL.key) into the keystore's layout. Only the unencrypted ssh-ed25519 form is
 * supported for now: an encrypted key (bcrypt KDF, any cipher), an RSA, or an ECDSA key gives NEMA_ERR_UNSUPPORTED, so the
 * caller can say so. The passphrase is accepted by the API and not used until encrypted keys are supported. */
#include <string.h>

#include "../../third_party/monocypher/monocypher.h"
#include "crypto_prim.h"
#include "crypto_tab.h"
#include "keystore.h"
#include "nema/types.h"

#define BEGIN "-----BEGIN OPENSSH PRIVATE KEY-----"
#define END "-----END OPENSSH PRIVATE KEY-----"
#define MAGIC "openssh-key-v1"
#define MAX_BIN 2048

static size_t find(const uint8_t *s, size_t n, const char *pat, size_t from) {
    size_t m = strlen(pat);
    for (size_t i = from; i + m <= n; i++)
        if (memcmp(s + i, pat, m) == 0) return i;
    return n;
}

static int b64val(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Base64 with any whitespace skipped; stops at '='. */
static int b64_decode(const uint8_t *s, size_t n, uint8_t *out, size_t cap, size_t *len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=') break;
        if (s[i] == ' ' || s[i] == '\r' || s[i] == '\n' || s[i] == '\t') continue;
        int v = b64val(s[i]);
        if (v < 0) return NEMA_ERR_INVAL;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return NEMA_ERR_RANGE;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    *len = o;
    return NEMA_OK;
}

/* A cursor over SSH wire data: uint32 big-endian, and strings (uint32 length then bytes). */
typedef struct {
    const uint8_t *p;
    size_t n;
} rd_t;

static int rd_u32(rd_t *r, uint32_t *v) {
    if (r->n < 4) return NEMA_ERR_INVAL;
    *v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
    r->p += 4, r->n -= 4;
    return NEMA_OK;
}

static int rd_str(rd_t *r, const uint8_t **s, uint32_t *len) {
    uint32_t l;
    if (rd_u32(r, &l) != NEMA_OK || l > r->n) return NEMA_ERR_INVAL;
    *s = r->p, *len = l;
    r->p += l, r->n -= l;
    return NEMA_OK;
}

static int str_is(const uint8_t *s, uint32_t n, const char *lit) { return n == strlen(lit) && memcmp(s, lit, n) == 0; }

int kf_import_openssh(const uint8_t *text, size_t len, uint8_t *type, uint8_t plain[KS_PLAIN_LEN]) {
    size_t b = find(text, len, BEGIN, 0);
    if (b == len) return NEMA_ERR_INVAL;
    size_t body = b + strlen(BEGIN);
    size_t e = find(text, len, END, body);
    if (e == len) return NEMA_ERR_INVAL;
    uint8_t bin[MAX_BIN];
    size_t bl = 0;
    int rc = b64_decode(text + body, e - body, bin, sizeof(bin), &bl);
    if (rc != NEMA_OK) return rc;
    if (bl < strlen(MAGIC) + 1 || memcmp(bin, MAGIC, strlen(MAGIC) + 1) != 0) return NEMA_ERR_INVAL;

    rd_t r = {bin + strlen(MAGIC) + 1, bl - strlen(MAGIC) - 1};
    const uint8_t *cipher, *kdf, *kdfopts, *pubblob, *privblob;
    uint32_t cl, kl, ol, nkeys, pl, prl;
    if (rd_str(&r, &cipher, &cl) || rd_str(&r, &kdf, &kl) || rd_str(&r, &kdfopts, &ol) || rd_u32(&r, &nkeys)) return NEMA_ERR_INVAL;
    if (!str_is(cipher, cl, "none") || !str_is(kdf, kl, "none")) return NEMA_ERR_UNSUPPORTED; /* encrypted: not yet */
    if (nkeys != 1) return NEMA_ERR_UNSUPPORTED;
    if (rd_str(&r, &pubblob, &pl) || rd_str(&r, &privblob, &prl)) return NEMA_ERR_INVAL;

    rd_t pub = {pubblob, pl};
    const uint8_t *ktype, *pk;
    uint32_t ktl, pkl;
    if (rd_str(&pub, &ktype, &ktl) || rd_str(&pub, &pk, &pkl)) return NEMA_ERR_INVAL;
    if (!str_is(ktype, ktl, "ssh-ed25519")) return NEMA_ERR_UNSUPPORTED;
    if (pkl != 32) return NEMA_ERR_INVAL;

    rd_t pr = {privblob, prl};
    uint32_t c1, c2;
    const uint8_t *ktype2, *pk2, *sk;
    uint32_t ktl2, pkl2, skl;
    if (rd_u32(&pr, &c1) || rd_u32(&pr, &c2) || c1 != c2) return NEMA_ERR_INVAL; /* check words: a damaged file */
    if (rd_str(&pr, &ktype2, &ktl2) || rd_str(&pr, &pk2, &pkl2) || rd_str(&pr, &sk, &skl)) return NEMA_ERR_INVAL;
    if (!str_is(ktype2, ktl2, "ssh-ed25519") || pkl2 != 32 || skl != 64) return NEMA_ERR_INVAL;
    if (memcmp(pk, pk2, 32) != 0 || memcmp(sk + 32, pk, 32) != 0) return NEMA_ERR_INVAL;

    /* The seed must produce exactly this key pair, so a wrong or swapped key cannot be stored. */
    uint8_t sk_check[64], pk_check[32];
    prim_ed25519_keypair(sk_check, pk_check, sk);
    int same = memcmp(sk_check, sk, 64) == 0 && memcmp(pk_check, pk, 32) == 0;
    crypto_wipe(sk_check, sizeof(sk_check));
    if (!same) return NEMA_ERR_INVAL;

    memset(plain, 0, KS_PLAIN_LEN);
    memcpy(plain, sk, 64);
    memcpy(plain + 64, pk, 32);
    *type = CT_KEY_ED25519;
    crypto_wipe(bin, sizeof(bin));
    return NEMA_OK;
}
