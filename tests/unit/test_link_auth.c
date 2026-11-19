/* Palanu Link pairing, authentication and trust (docs/PLP.md), on the host. The state machine runs with a fake clock and
 * fake randomness; the links run for real (frames in, frames out) with a transmit hook that records the reply bytes.
 * The client side of the handshake is written out here from the spec, so the test checks the wire against the spec,
 * not against the device's own code. Covers: the pairing window (wrong codes, timeout, single use), the token stored
 * encrypted and refused when altered, auth success, failure and back-off, revocation dropping a live session, the
 * untrusted channel refusals, the trusted cable, and the eight-client limit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../system/src/crypto_mac.h"
#include "../../system/src/crypto_prim.h"
#include "../../system/src/cryptoimpl.h"
#include "../../system/src/link_auth.h"
#include "../../system/src/link_clients.h"
#include "../../system/src/link_int.h"
#include "../../third_party/monocypher/monocypher.h"
#include "nema/nema.h"
#include "nema/vfs.h"
#include "pal/link.h"
#include "test.h"

#define CH_CTL 0
#define CH_TTY 1
#define CH_SCREEN 2
#define CH_INPUT 3
#define CH_FS 4
#define CH_OTA 5
#define T_HELLO 1
#define T_PING 3
#define T_ERR 0x7F
#define T_TTY_OPEN 1
#define T_FS_LIST 1
#define T_SCR_SUB 1
#define T_OTA_BEGIN 1
#define T_KEY 1
#define T_FS_ERR 0x7F

static char ROOT[256];

/* ---- fake clock and randomness ---- */

static uint32_t g_now = 1000000u;
static uint32_t fake_now(void) { return g_now; }
static uint64_t g_rs = 0x9e3779b97f4a7c15ull;
static void fake_random(void *buf, size_t n) {
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        g_rs ^= g_rs << 13;
        g_rs ^= g_rs >> 7;
        g_rs ^= g_rs << 17;
        p[i] = (uint8_t)(g_rs >> 24);
    }
}

/* ---- the transmit hook and the frames it carries ---- */

typedef struct {
    uint8_t chan, type;
    uint8_t p[1100];
    size_t n;
} frame_t;

static uint8_t g_tx[16384];
static size_t g_txn;
static frame_t g_fr[64];
static int g_nfr;

static int fake_tx(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx;
    if (g_txn + n > sizeof(g_tx)) return -1;
    memcpy(g_tx + g_txn, d, n);
    g_txn += n;
    return 0;
}

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Moves the bytes the link wrote into g_fr. Called after each feed. */
static void take(void) {
    g_nfr = 0;
    size_t i = 0;
    while (i + 7 <= g_txn) {
        if (g_tx[i] != 0xAB) {
            i++;
            continue;
        }
        size_t len = g_tx[i + 3] | (g_tx[i + 4] << 8);
        if (i + 7 + len > g_txn) break;
        uint16_t want = (uint16_t)(g_tx[i + 5 + len] | (g_tx[i + 6 + len] << 8));
        if (crc16(g_tx + i + 1, 4 + len) == want && g_nfr < 64) {
            frame_t *f = &g_fr[g_nfr++];
            f->chan = g_tx[i + 1];
            f->type = g_tx[i + 2];
            f->n = len;
            memcpy(f->p, g_tx + i + 5, len);
        }
        i += 7 + len;
    }
    g_txn = 0;
}

static const frame_t *find(uint8_t chan, uint8_t type) {
    for (int i = 0; i < g_nfr; i++)
        if (g_fr[i].chan == chan && g_fr[i].type == type) return &g_fr[i];
    return NULL;
}

/* The device's error code is an int8 in the first payload byte. */
static int code_of(const frame_t *f) { return (int8_t)f->p[0]; }

static void feed(pal_link_t *l, uint8_t chan, uint8_t type, const uint8_t *p, size_t n) {
    uint8_t f[7 + 1100];
    f[0] = 0xAB;
    f[1] = chan;
    f[2] = type;
    f[3] = (uint8_t)n;
    f[4] = (uint8_t)(n >> 8);
    if (n) memcpy(f + 5, p, n);
    uint16_t c = crc16(f + 1, 4 + n);
    f[5 + n] = (uint8_t)c;
    f[6 + n] = (uint8_t)(c >> 8);
    pal_link_feed(l, f, 7 + n);
    take();
}

static pal_link_t *make_link(pal_link_transport_t tr) {
    pal_link_io_t io = {.tx = fake_tx, .ctx = NULL, .transport = tr};
    pal_link_t *l = pal_link_create(&io);
    take();
    if (!l) {
        printf("  link create failed\n");
        abort();
    }
    return l;
}

/* ---- the client side of the handshake (docs/PLP.md, written from the spec) ---- */

typedef struct {
    uint8_t csk[32], cpub[32], cn[16], dpub[32], dn[16], shared[32];
} client_t;

static void cl_hmac(uint8_t out[32], const uint8_t *key, size_t klen, const uint8_t *a, size_t na, const uint8_t *b,
                    size_t nb, const uint8_t *c, size_t nc) {
    crypto_mac_t m;
    crypto_mac_hmac(&m, CI_HASH_SHA256, key, klen);
    if (na) crypto_mac_update(&m, a, na);
    if (nb) crypto_mac_update(&m, b, nb);
    if (nc) crypto_mac_update(&m, c, nc);
    crypto_mac_final(&m, out);
}

/* PAIR_REQUEST, then the device's PAIR_CHALLENGE. Returns the reply's error code, or 0. */
static int cl_request(pal_link_t *l, client_t *c, const char *name) {
    fake_random(c->csk, 32);
    fake_random(c->cn, 16);
    prim_x25519_base(c->cpub, c->csk);
    uint8_t req[1 + 24 + 48];
    size_t nl = strlen(name);
    req[0] = (uint8_t)nl;
    memcpy(req + 1, name, nl);
    memcpy(req + 1 + nl, c->cpub, 32);
    memcpy(req + 1 + nl + 32, c->cn, 16);
    feed(l, CH_CTL, 5, req, 1 + nl + 48);
    const frame_t *ch = find(CH_CTL, 6);
    if (!ch) return find(CH_CTL, T_ERR) ? code_of(find(CH_CTL, T_ERR)) : -999;
    memcpy(c->dpub, ch->p, 32);
    memcpy(c->dn, ch->p + 32, 16);
    return prim_x25519(c->shared, c->csk, c->dpub) == NEMA_OK ? 0 : -998;
}

/* PAIR_PROOF with a code. On success fills id and token and returns 0; otherwise the device's error code. */
static int cl_proof(pal_link_t *l, client_t *c, const char *code, uint8_t id[8], uint8_t token[32]) {
    uint8_t tr[96], prk[32], km[32], ke[32], proof[32];
    memcpy(tr, c->cpub, 32);
    memcpy(tr + 32, c->dpub, 32);
    memcpy(tr + 64, c->cn, 16);
    memcpy(tr + 80, c->dn, 16);
    cl_hmac(prk, (const uint8_t *)"palanu-pair-v1", 14, c->shared, 32, (const uint8_t *)code, 6, NULL, 0);
    cl_hmac(km, prk, 32, (const uint8_t *)"mac", 3, tr, 96, NULL, 0);
    cl_hmac(ke, prk, 32, (const uint8_t *)"enc", 3, tr, 96, NULL, 0);
    cl_hmac(proof, km, 32, (const uint8_t *)"pair-proof", 10, tr, 96, NULL, 0);
    feed(l, CH_CTL, 7, proof, 32);
    const frame_t *ok = find(CH_CTL, 8);
    if (!ok) return find(CH_CTL, T_ERR) ? code_of(find(CH_CTL, T_ERR)) : -994;
    memcpy(id, ok->p, 8);
    uint8_t nonce[12], aad[20];
    memcpy(nonce, ok->p + 8, 12);
    memcpy(aad, "palanu-token", 12);
    memcpy(aad + 12, id, 8);
    memcpy(token, ok->p + 20, 32);
    int rc = prim_aead_open(ke, nonce, aad, sizeof(aad), token, 32, ok->p + 52);
    return rc == NEMA_OK ? 0 : -997;
}

/* AUTH_HELLO, AUTH_CHALLENGE, AUTH_RESPONSE. Returns 0 when AUTH_OK came back, else the error code. */
static int cl_auth_with(pal_link_t *l, const uint8_t id[8], const uint8_t token[32], pal_link_transport_t tr) {
    feed(l, CH_CTL, 9, id, 8);
    const frame_t *ch = find(CH_CTL, 10);
    if (!ch) return find(CH_CTL, T_ERR) ? code_of(find(CH_CTL, T_ERR)) : -993;
    uint8_t nonce[16], tag = (uint8_t)tr, mac[32];
    memcpy(nonce, ch->p, 16);
    cl_hmac(mac, token, 32, nonce, 16, id, 8, &tag, 1);
    feed(l, CH_CTL, 11, mac, 32);
    if (find(CH_CTL, 12)) return 0;
    return find(CH_CTL, T_ERR) ? code_of(find(CH_CTL, T_ERR)) : -996;
}

static int cl_auth(pal_link_t *l, const uint8_t id[8], const uint8_t token[32]) {
    return cl_auth_with(l, id, token, PAL_LINK_TCP);
}

/* A full pairing over `l` with a window opened now; returns the device's error (0 on success). */
static int pair_now(pal_link_t *l, const char *name, uint8_t id[8], uint8_t token[32], char code[7]) {
    if (pal_link_pair_open(code) != NEMA_OK) return -995;
    client_t c;
    int rc = cl_request(l, &c, name);
    if (rc) return rc;
    return cl_proof(l, &c, code, id, token);
}

static void hex(const uint8_t *b, size_t n, char *out) { lc_hex_enc(b, n, out); }


static void reset_store(void) {
    /* a fresh device: no clients, no config, no rate limit; the store directory is left for the next test */
    uint8_t ids[LC_MAX][8];
    int n = lc_ids(ids, LC_MAX);
    for (int i = 0; i < n; i++) lc_remove(ids[i]);
    la_backoff_reset();
    la_pair_close();
    pal_link_uart_trust(true);
}

/* ---- tests ---- */

TEST(pairing_wrong_codes_use_up_the_window) {
    reset_store();
    pal_link_t *l = make_link(PAL_LINK_TCP);
    feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    char code[7], wrong[7];
    la_pair_open(code); /* a window with a fresh code */
    memcpy(wrong, "000000", 7);
    if (!strcmp(code, wrong)) memcpy(wrong, "111111", 7);
    client_t c;
    CHECK_EQ(cl_request(l, &c, "phone"), 0);
    uint8_t id[8], tok[32];
    CHECK_EQ(cl_proof(l, &c, wrong, id, tok), NEMA_ERR_AUTH);
    const frame_t *e = find(CH_CTL, T_ERR);
    CHECK(e && strstr((const char *)e->p + 1, "2 tries left"));
    CHECK_EQ(cl_proof(l, &c, wrong, id, tok), NEMA_ERR_AUTH);
    CHECK_EQ(cl_proof(l, &c, wrong, id, tok), PAL_LINK_E_PAIR_CLOSED); /* third wrong code closes the window */
    CHECK_EQ(cl_proof(l, &c, code, id, tok), PAL_LINK_E_PAIR_CLOSED);  /* the right code is too late */
    /* reconnecting does not reset the tries */
    pal_link_t *l2 = make_link(PAL_LINK_TCP);
    CHECK_EQ(cl_request(l2, &c, "phone"), PAL_LINK_E_PAIR_CLOSED);
    CHECK_EQ(lc_count(), 0);
    pal_link_destroy(l2);
    pal_link_destroy(l);
}

TEST(pairing_window_times_out_on_the_clock) {
    reset_store();
    char code[7];
    la_pair_open(code);
    g_now += 119999u;
    pal_link_t *l = make_link(PAL_LINK_TCP);
    client_t c;
    CHECK_EQ(cl_request(l, &c, "phone"), 0); /* still open one millisecond before the end */
    pal_link_destroy(l);
    g_now += 2u;
    la_pair_open(code);
    g_now += 120000u;
    l = make_link(PAL_LINK_TCP);
    CHECK_EQ(cl_request(l, &c, "phone"), PAL_LINK_E_PAIR_CLOSED);
    char st[96];
    pal_link_pair_status(st, sizeof(st));
    CHECK(strstr(st, "closed") != NULL);
    pal_link_destroy(l);
}

TEST(pairing_code_is_single_use_and_token_round_trips) {
    reset_store();
    pal_link_t *l = make_link(PAL_LINK_TCP);
    feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    uint8_t id[8], tok[32];
    char code[7];
    CHECK_EQ(pair_now(l, "laptop", id, tok, code), 0);
    CHECK_EQ(lc_count(), 1);
    client_t c;
    CHECK_EQ(cl_request(l, &c, "laptop"), PAL_LINK_E_PAIR_CLOSED); /* the window closed on success */
    lc_rec_t rec;
    CHECK_EQ(lc_load(id, &rec), NEMA_OK);
    CHECK(memcmp(rec.token, tok, 32) == 0);
    CHECK(strcmp(rec.name, "laptop") == 0);
    pal_link_destroy(l);
}

TEST(token_is_encrypted_at_rest_and_altered_records_are_refused) {
    reset_store();
    pal_link_t *l = make_link(PAL_LINK_TCP);
    uint8_t id[8], tok[32];
    char code[7];
    CHECK_EQ(pair_now(l, "rec", id, tok, code), 0);
    pal_link_destroy(l);
    char hexid[17], path[320];
    hex(id, 8, hexid);
    snprintf(path, sizeof(path), "%s/data/link/clients/%s.cli", ROOT, hexid);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    uint8_t raw[256];
    size_t n = f ? fread(raw, 1, sizeof(raw), f) : 0;
    if (f) fclose(f);
    CHECK(n > 0);
    CHECK(memmem(raw, n, tok, 32) == NULL); /* the token is not in the file */
    CHECK(memmem(raw, n, "rec", 3) == NULL); /* nor the name */
    lc_rec_t rec;
    CHECK_EQ(lc_load(id, &rec), NEMA_OK);
    raw[n / 2] ^= 0x01; /* alter one byte of the sealed record */
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite(raw, 1, n, f);
        fclose(f);
    }
    CHECK_EQ(lc_load(id, &rec), NEMA_ERR_AUTH);
}

TEST(auth_succeeds_then_fails_then_backs_off_for_30_seconds) {
    reset_store();
    pal_link_t *l = make_link(PAL_LINK_TCP);
    uint8_t id[8], tok[32];
    char code[7];
    CHECK_EQ(pair_now(l, "phone", id, tok, code), 0);
    pal_link_destroy(l);

    l = make_link(PAL_LINK_TCP);
    feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    CHECK(find(CH_CTL, 2) && strstr((const char *)find(CH_CTL, 2)->p, "\"trusted\":false"));
    CHECK(!pal_link_connected());
    uint8_t wrong[32];
    memset(wrong, 0x5a, sizeof(wrong));
    CHECK_EQ(cl_auth(l, id, wrong), NEMA_ERR_AUTH);
    CHECK(!l->trusted);
    pal_link_destroy(l);

    /* five failed responses on TCP: the sixth attempt is refused at once, even with the right token */
    for (int i = 0; i < 4; i++) { /* with the first attempt above, five failures in all */
        pal_link_t *b = make_link(PAL_LINK_TCP);
        feed(b, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
        CHECK_EQ(cl_auth(b, id, wrong), NEMA_ERR_AUTH);
        pal_link_destroy(b);
    }
    l = make_link(PAL_LINK_TCP);
    feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    CHECK_EQ(cl_auth(l, id, tok), NEMA_ERR_BUSY);
    CHECK(!l->trusted);
    pal_link_destroy(l);

    /* the back-off is per transport: the cable is not held back */
    pal_link_t *u = make_link(PAL_LINK_UART);
    feed(u, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    CHECK(u->trusted);
    pal_link_destroy(u);

    g_now += 30000u;
    l = make_link(PAL_LINK_TCP);
    feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    CHECK_EQ(cl_auth(l, id, tok), 0);
    CHECK(l->trusted);
    CHECK(pal_link_connected());
    pal_link_destroy(l);
}

TEST(untrusted_links_refuse_every_channel_but_hello_ping_and_pairing) {
    reset_store();
    const pal_link_transport_t kinds[2] = {PAL_LINK_TCP, PAL_LINK_BLE};
    for (int k = 0; k < 2; k++) {
        pal_link_t *l = make_link(kinds[k]);
        feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
        const frame_t *info = find(CH_CTL, 2);
        CHECK(info != NULL);
        CHECK(info && strstr((const char *)info->p, "\"trusted\":false") != NULL);
        CHECK(info && strstr((const char *)info->p, "\"chip\"") == NULL); /* basic info only */
        feed(l, CH_CTL, T_PING, (const uint8_t *)"abcd", 4);
        CHECK(find(CH_CTL, 4) != NULL);
        feed(l, CH_TTY, T_TTY_OPEN, (const uint8_t *)"\x50\x00\x18\x00", 4);
        CHECK(find(CH_TTY, T_ERR) && find(CH_TTY, T_ERR)->p[0] == (uint8_t)NEMA_ERR_AUTH);
        CHECK(l->tty == NULL); /* no shell was opened */
        uint8_t fsreq[6] = {7, '/', 'h', 'o', 'm', 'e'};
        feed(l, CH_FS, T_FS_LIST, fsreq, sizeof(fsreq));
        CHECK(find(CH_FS, T_FS_ERR) && find(CH_FS, T_FS_ERR)->p[1] == (uint8_t)NEMA_ERR_AUTH);
        feed(l, CH_SCREEN, T_SCR_SUB, (const uint8_t *)"\x01", 1);
        CHECK(find(CH_SCREEN, T_ERR) && find(CH_SCREEN, T_ERR)->p[0] == (uint8_t)NEMA_ERR_AUTH);
        CHECK(!l->screen_on);
        feed(l, CH_INPUT, T_KEY, (const uint8_t *)"\x02\x00\x00\x00\x00\x00", 6);
        CHECK(find(CH_INPUT, T_ERR) && find(CH_INPUT, T_ERR)->p[0] == (uint8_t)NEMA_ERR_AUTH);
        feed(l, CH_OTA, T_OTA_BEGIN, (const uint8_t *)"\x10\x00\x00\x00", 4);
        CHECK(find(CH_OTA, T_ERR) && find(CH_OTA, T_ERR)->p[0] == (uint8_t)NEMA_ERR_AUTH);
        CHECK(!l->ota_open);
        pal_link_destroy(l);
    }
}

TEST(cable_is_trusted_by_default_and_trust_can_be_switched_off_live) {
    reset_store();
    pal_link_t *u = make_link(PAL_LINK_UART);
    feed(u, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    CHECK(find(CH_CTL, 2) && strstr((const char *)find(CH_CTL, 2)->p, "\"trusted\":true"));
    uint8_t fsreq[2] = {1, '/'};
    feed(u, CH_FS, T_FS_LIST, fsreq, sizeof(fsreq));
    CHECK(find(CH_FS, 3) != NULL); /* END: the listing ran */
    CHECK(!find(CH_FS, T_FS_ERR));

    CHECK_EQ(pal_link_uart_trust(false), NEMA_OK);
    CHECK(!pal_link_uart_trusted());
    feed(u, CH_FS, T_FS_LIST, fsreq, sizeof(fsreq));
    CHECK(find(CH_FS, T_FS_ERR) && find(CH_FS, T_FS_ERR)->p[1] == (uint8_t)NEMA_ERR_AUTH);
    CHECK(!u->trusted);

    CHECK_EQ(pal_link_uart_trust(true), NEMA_OK);
    feed(u, CH_FS, T_FS_LIST, fsreq, sizeof(fsreq));
    CHECK(find(CH_FS, 3) != NULL);
    pal_link_destroy(u);
}

TEST(revoking_a_client_drops_its_live_session) {
    reset_store();
    pal_link_t *l = make_link(PAL_LINK_TCP);
    feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    uint8_t id[8], tok[32];
    char code[7];
    CHECK_EQ(pair_now(l, "tablet", id, tok, code), 0);
    CHECK_EQ(cl_auth(l, id, tok), 0);
    CHECK(pal_link_connected());
    /* the shell may open on the authenticated link: not refused (the host has no shell to start) */
    feed(l, CH_TTY, T_TTY_OPEN, (const uint8_t *)"\x50\x00\x18\x00", 4);
    CHECK(!(find(CH_TTY, T_ERR) && code_of(find(CH_TTY, T_ERR)) == NEMA_ERR_AUTH));

    char hexid[17];
    hex(id, 8, hexid);
    CHECK_EQ(pal_link_revoke(hexid), NEMA_OK);
    CHECK(l->kill); /* the TCP loop closes the socket on this */
    feed(l, CH_CTL, T_PING, (const uint8_t *)"abcd", 4); /* the next frame ends the session */
    CHECK(!l->trusted);
    CHECK(l->tty == NULL);
    CHECK(!pal_link_connected());
    feed(l, CH_FS, T_FS_LIST, (const uint8_t *)"\x01/", 2);
    CHECK(find(CH_FS, T_FS_ERR) && find(CH_FS, T_FS_ERR)->p[1] == (uint8_t)NEMA_ERR_AUTH);
    pal_link_destroy(l);

    /* the same token no longer authenticates on a new connection */
    pal_link_t *n = make_link(PAL_LINK_TCP);
    feed(n, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    CHECK(cl_auth(n, id, tok) != 0);
    CHECK(!n->trusted);
    pal_link_destroy(n);
    CHECK_EQ(pal_link_revoke(hexid), NEMA_ERR_NOENT);
    CHECK_EQ(pal_link_revoke("not-hex"), NEMA_ERR_INVAL);
}

/* A browser's "Forget" (AUTH_FORGET): the device deletes the pairing the connection signed in with, answers, and ends the session.
 * A link that did not sign in with a pairing (the cable) has nothing to forget. */
TEST(a_client_can_ask_the_device_to_forget_it) {
    reset_store();
    pal_link_t *l = make_link(PAL_LINK_WS);
    feed(l, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    uint8_t id[8], tok[32];
    char code[7];
    CHECK_EQ(pair_now(l, "Chrome on macOS", id, tok, code), 0);
    CHECK_EQ(cl_auth_with(l, id, tok, PAL_LINK_WS), 0);
    CHECK_EQ(lc_count(), 1);
    feed(l, CH_CTL, LA_TYPE_AUTH_FORGET, NULL, 0);
    CHECK(find(CH_CTL, LA_TYPE_AUTH_FORGOTTEN) != NULL);
    CHECK_EQ(lc_count(), 0);
    CHECK(l->kill);
    pal_link_destroy(l);

    pal_link_t *u = make_link(PAL_LINK_UART); /* trusted by the cable, signed in as nobody */
    feed(u, CH_CTL, T_HELLO, (const uint8_t *)"t", 1);
    feed(u, CH_CTL, LA_TYPE_AUTH_FORGET, NULL, 0);
    CHECK(find(CH_CTL, LA_TYPE_AUTH_FORGOTTEN) == NULL && find(CH_CTL, T_ERR) != NULL);
    pal_link_destroy(u);
}

TEST(eight_clients_at_most) {
    reset_store();
    pal_link_t *l = make_link(PAL_LINK_TCP);
    uint8_t id[8], tok[32];
    char code[7];
    for (int i = 0; i < LC_MAX; i++) CHECK_EQ(pair_now(l, "dev", id, tok, code), 0);
    CHECK_EQ(lc_count(), LC_MAX);
    CHECK_EQ(pal_link_pair_open(code), NEMA_OK);
    client_t c;
    CHECK_EQ(cl_request(l, &c, "ninth"), NEMA_ERR_FULL);
    CHECK_EQ(pal_link_revoke("all"), NEMA_OK);
    CHECK_EQ(lc_count(), 0);
    pal_link_destroy(l);
}

/* Leaves the stack below the caller full of hex digits: the id's reader once measured its buffer past the 16 digits it read, which
 * only went unnoticed while that stack happened to be zero (it was not on the board: every hello made a new id). */
static void __attribute__((noinline)) dirty_stack(void) {
    volatile char junk[2048];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = 'a';
}

TEST(device_id_is_stable_and_clients_list_names_them) {
    reset_store();
    char a[17], b[17], c[17];
    pal_link_device_id(a);
    dirty_stack();
    pal_link_device_id(b);
    dirty_stack();
    pal_link_device_id(c);
    CHECK(strlen(a) == 16 && strcmp(a, b) == 0 && strcmp(b, c) == 0);
    pal_link_t *l = make_link(PAL_LINK_TCP);
    uint8_t id[8], tok[32];
    char code[7];
    CHECK_EQ(pair_now(l, "watch", id, tok, code), 0);
    char text[512];
    size_t n = pal_link_clients_text(text, sizeof(text));
    CHECK(n > 0 && strstr(text, "watch") != NULL);
    pal_link_destroy(l);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(ROOT, sizeof(ROOT), "%s/palanu-link-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(ROOT)) abort();
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    void *fs = nema_posixfs_create(ROOT);
    if (!fs) abort();
    nema_vfs_mount("/", nema_posixfs_ops(), fs);
    nema_fs_mkdir("/data");
    la_env_t env = {fake_now, fake_random};
    la_set_env(&env);
    RUN(pairing_wrong_codes_use_up_the_window);
    RUN(pairing_window_times_out_on_the_clock);
    RUN(pairing_code_is_single_use_and_token_round_trips);
    RUN(token_is_encrypted_at_rest_and_altered_records_are_refused);
    RUN(auth_succeeds_then_fails_then_backs_off_for_30_seconds);
    RUN(untrusted_links_refuse_every_channel_but_hello_ping_and_pairing);
    RUN(cable_is_trusted_by_default_and_trust_can_be_switched_off_live);
    RUN(revoking_a_client_drops_its_live_session);
    RUN(a_client_can_ask_the_device_to_forget_it);
    RUN(eight_clients_at_most);
    RUN(device_id_is_stable_and_clients_list_names_them);
    return TEST_DONE();
}
