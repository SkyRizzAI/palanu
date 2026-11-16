/* Host tests of the SSH library (sdk/palssh, T4.1 to T4.3): base64, fingerprints, the wire format, known_hosts, the
 * exchange hash and key derivation against vectors from tests/unit/tools/palssh_vectors.py, the packet protections
 * (aes128/256-gcm and chacha20-poly1305, both directions, with tampering), and a complete handshake against an
 * in-test server that speaks the peer side of RFC 4253. The backend is built on the system's own primitives
 * (system/src/crypto_*), the code the simulator runs. Nothing here touches sockets or the clock. */
#include <stdio.h>
#include <string.h>

#include "../../system/src/cryptoimpl.h"
#include "../../system/src/crypto_mac.h"
#include "../../system/src/crypto_prim.h"
#include "nema/nema.h"
#define PALSSH_IMPLEMENTATION
#include "palssh/palssh.h"
#include "palssh_vectors.h"
#include "test.h"

#define T_QUEUE 65536

#include "palssh_peer.inc"

/* ---- tests ---- */

static tnet T;     /* the network and the in-test server */

static void net_reset(void) {
    memset(&T, 0, sizeof T);
    T.rnd = 12345u;
    T.state = P_IDENT;
}

static int blob_of_key(const uint8_t pk[32], uint8_t *out, size_t cap);

/* The server's host key: a fixed ed25519 key (RFC 8032 style seed) and its SSH blob. */
static void host_key_setup(void) {
    uint8_t seed[32];
    memset(seed, 0x42, sizeof seed);
    prim_ed25519_keypair(T.sk, T.pk, seed);
    T.host_blob_n = (size_t)blob_of_key(T.pk, T.host_blob, sizeof T.host_blob);
}

static int blob_of_key(const uint8_t pk[32], uint8_t *out, size_t cap) {
    pssh_wb_t b;
    wb_init(&b, out, cap);
    wb_cstr(&b, "ssh-ed25519");
    wb_str(&b, pk, 32);
    return b.err ? -1 : (int)b.n;
}

TEST(base64_vectors) {
    char out[32];
    CHECK_EQ(pssh_b64_encode((const uint8_t *)"foobar", 6, out, sizeof out), 8);
    CHECK(strcmp(out, "Zm9vYmFy") == 0);
    CHECK(strcmp((pssh_b64_encode((const uint8_t *)"fo", 2, out, sizeof out), out), "Zm8=") == 0);
    CHECK(strcmp((pssh_b64_encode((const uint8_t *)"f", 1, out, sizeof out), out), "Zg==") == 0);
    uint8_t back[16];
    CHECK_EQ(pssh_b64_decode("Zm9vYmFy", 8, back, sizeof back), 6);
    CHECK(memcmp(back, "foobar", 6) == 0);
    CHECK_EQ(pssh_b64_decode("Zg==", 4, back, sizeof back), 1);
    CHECK_EQ(pssh_b64_decode("Zm9v!", 5, back, sizeof back), PSSH_E_PROTO);
}

TEST(wire_writer_and_reader) {
    uint8_t buf[64];
    pssh_wb_t b;
    wb_init(&b, buf, sizeof buf);
    wb_mpint(&b, (const uint8_t *)"\0\0", 2); /* zero */
    wb_mpint(&b, (const uint8_t *)"\x80", 1); /* top bit set: 0x00 prefix */
    wb_mpint(&b, (const uint8_t *)"\0\x01", 2); /* leading zero dropped */
    const uint8_t want[] = {0, 0, 0, 0, 0, 0, 0, 2, 0, 0x80, 0, 0, 0, 1, 1};
    CHECK_EQ(b.n, sizeof want);
    CHECK(memcmp(buf, want, sizeof want) == 0);
    pssh_rb_t r;
    rb_init(&r, buf, b.n);
    pssh_bytes_t z = rb_str(&r), hi = rb_str(&r), one = rb_str(&r);
    CHECK_EQ(z.n, 0);
    CHECK_EQ(hi.n, 2);
    CHECK_EQ(one.n, 1);
    CHECK(!r.err);
    rb_str(&r);
    CHECK(r.err); /* past the end */
    wb_init(&b, buf, 4);
    wb_u32(&b, 1);
    wb_u32(&b, 2);
    CHECK(b.err); /* overflow is an error, nothing is half written */
}

TEST(fingerprint_and_host_blob) {
    uint8_t blob[128], b64[256];
    int n = blob_of_key(vec_pk, blob, sizeof blob);
    CHECK(n > 0);
    CHECK(pssh_b64_encode(blob, (size_t)n, (char *)b64, sizeof b64) > 0);
    CHECK(strcmp((const char *)b64, VEC_HOST_BLOB_B64) == 0);
    tnet t;
    memset(&t, 0, sizeof t);
    pssh_ops_t o = tb_ops(&t);
    char fp[80];
    CHECK(pssh_fingerprint(&o, blob, (size_t)n, fp, sizeof fp) > 0);
    CHECK(strcmp(fp, VEC_FINGERPRINT) == 0);
    CHECK_EQ(pssh_fingerprint(&o, blob, (size_t)n, fp, 10), PSSH_E_ARG);
}

TEST(exchange_hash_and_key_derivation_vectors) {
    uint8_t ks[128], h[32], k[32];
    int kn = pssh_b64_decode(VEC_HOST_BLOB_B64, pssh_len(VEC_HOST_BLOB_B64), ks, sizeof ks);
    CHECK(kn > 0);
    memcpy(k, vec_K, 32);
    tnet t;
    memset(&t, 0, sizeof t);
    pssh_ops_t o = tb_ops(&t);
    pssh_bytes_t parts[7] = {
        {(const uint8_t *)"SSH-2.0-PalanuOS_1.0", 20},
        {(const uint8_t *)"SSH-2.0-OpenSSH_9.6", 19},
        {vec_I_C, sizeof vec_I_C},
        {vec_I_S, sizeof vec_I_S},
        {ks, (size_t)kn},
        {vec_Q_C, 32},
        {vec_Q_S, 32},
    };
    CHECK_EQ(pssh_exchange_hash(&o, parts, k, h), PSSH_OK);
    CHECK(memcmp(h, vec_H, 32) == 0);
    uint8_t out[64];
    CHECK_EQ(pssh_derive(&o, k, vec_H, vec_H, 'C', out, 64), PSSH_OK);
    CHECK(memcmp(out, vec_chacha_C, 64) == 0);
    CHECK_EQ(pssh_derive(&o, k, vec_H, vec_H, 'D', out, 64), PSSH_OK);
    CHECK(memcmp(out, vec_chacha_D, 64) == 0);
    CHECK_EQ(pssh_derive(&o, k, vec_H, vec_H, 'C', out, 16), PSSH_OK);
    CHECK(memcmp(out, vec_gcm128_key_C, 16) == 0);
    CHECK_EQ(pssh_derive(&o, k, vec_H, vec_H, 'A', out, 12), PSSH_OK);
    CHECK(memcmp(out, vec_gcm128_iv_A, 12) == 0);
    CHECK_EQ(pssh_derive(&o, k, vec_H, vec_H, 'D', out, 32), PSSH_OK);
    CHECK(memcmp(out, vec_gcm256_key_D, 32) == 0);
    CHECK_EQ(pssh_derive(&o, k, vec_H, vec_H, 'B', out, 12), PSSH_OK);
    CHECK(memcmp(out, vec_gcm256_iv_B, 12) == 0);
}

/* Known hosts: plain, [host]:port, hashed, wildcard, negated, revoked, and another key type. */
TEST(known_hosts_lookup_and_lines) {
    uint8_t blob1[128], blob2[128], blob3[128];
    uint8_t pk2[32], pk3[32];
    memset(pk2, 0x33, 32);
    memset(pk3, 0x44, 32);
    int n1 = blob_of_key(vec_pk, blob1, sizeof blob1);
    int n2 = blob_of_key(pk2, blob2, sizeof blob2);
    int n3 = blob_of_key(pk3, blob3, sizeof blob3);
    char k1[256], k2[256], k3[256];
    pssh_b64_encode(blob1, (size_t)n1, k1, sizeof k1);
    pssh_b64_encode(blob2, (size_t)n2, k2, sizeof k2);
    pssh_b64_encode(blob3, (size_t)n3, k3, sizeof k3);
    char text[2048];
    snprintf(text, sizeof text,
             "example.test ssh-ed25519 %s\n"
             "# a comment line\n"
             "[example.test]:2222 ssh-ed25519 %s\n"
             "%s ssh-ed25519 %s\n"
             "%s ssh-ed25519 %s\n"
             "*.wild.test ssh-ed25519 %s\n"
             "!bad.corp.test,*.corp.test ssh-ed25519 %s\n"
             "@revoked example.revoked ssh-ed25519 %s\n"
             "ecdsa.test ecdsa-sha2-nistp256 %s\n",
             k1, k2, VEC_HASHED_PLAIN, k1, VEC_HASHED_PORT, k1, k2, k2, k1, k1);
    tnet t;
    memset(&t, 0, sizeof t);
    pssh_ops_t o = tb_ops(&t);
    size_t tl = pssh_len(text);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "example.test", 22, "ssh-ed25519", blob1, (size_t)n1), PSSH_HOST_FOUND);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "example.test", 22, "ssh-ed25519", blob2, (size_t)n2), PSSH_HOST_CHANGED);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "nothing.example.org", 22, "ssh-ed25519", blob1, (size_t)n1), PSSH_HOST_NEW);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "example.test", 2222, "ssh-ed25519", blob1, (size_t)n1), PSSH_HOST_FOUND);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "example.test", 2222, "ssh-ed25519", blob2, (size_t)n2), PSSH_HOST_FOUND);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "example.test", 2222, "ssh-ed25519", blob3, (size_t)n3), PSSH_HOST_CHANGED);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "a.wild.test", 22, "ssh-ed25519", blob2, (size_t)n2), PSSH_HOST_FOUND);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "a.wild.test", 22, "ssh-ed25519", blob1, (size_t)n1), PSSH_HOST_CHANGED);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "bad.corp.test", 22, "ssh-ed25519", blob2, (size_t)n2), PSSH_HOST_NEW);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "ok.corp.test", 22, "ssh-ed25519", blob2, (size_t)n2), PSSH_HOST_FOUND);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "example.revoked", 22, "ssh-ed25519", blob1, (size_t)n1), PSSH_HOST_NEW);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "ecdsa.test", 22, "ecdsa-sha2-nistp256", blob1, (size_t)n1), PSSH_HOST_FOUND);
    CHECK_EQ(pssh_hosts_lookup(&o, text, tl, "ecdsa.test", 22, "ssh-ed25519", blob1, (size_t)n1), PSSH_HOST_NEW);
    /* a hashed entry on its own */
    char hashed[512];
    snprintf(hashed, sizeof hashed, "%s ssh-ed25519 %s\n", VEC_HASHED_PLAIN, k1);
    CHECK_EQ(pssh_hosts_lookup(&o, hashed, pssh_len(hashed), "example.test", 22, "ssh-ed25519", blob1, (size_t)n1),
             PSSH_HOST_FOUND);
    CHECK_EQ(pssh_hosts_lookup(&o, hashed, pssh_len(hashed), "example.test", 22, "ssh-ed25519", blob2, (size_t)n2),
             PSSH_HOST_CHANGED);
    CHECK_EQ(pssh_hosts_lookup(&o, hashed, pssh_len(hashed), "other.test", 22, "ssh-ed25519", blob1, (size_t)n1),
             PSSH_HOST_NEW);
    /* lines we write */
    char line[512];
    CHECK_EQ(pssh_hosts_line("example.test", 22, "ssh-ed25519", blob1, (size_t)n1, line, sizeof line),
             (int)(pssh_len("example.test ssh-ed25519 ") + pssh_len(k1) + 1));
    CHECK(strcmp(line, "example.test ssh-ed25519 " VEC_HOST_BLOB_B64 "\n") == 0);
    pssh_hosts_line("example.test", 2222, "ssh-ed25519", blob1, (size_t)n1, line, sizeof line);
    CHECK(strncmp(line, "[example.test]:2222 ssh-ed25519 ", 32) == 0);
    /* a line we wrote is found again */
    CHECK_EQ(pssh_hosts_lookup(&o, line, pssh_len(line), "example.test", 2222, "ssh-ed25519", blob1, (size_t)n1),
             PSSH_HOST_FOUND);
}

TEST(identification_string) {
    static pssh_t s;
    tnet t;
    memset(&t, 0, sizeof t);
    pssh_ops_t o = tb_ops(&t);
    CHECK_EQ(pssh_init(&s, &o), PSSH_OK);
    const char *banner = "Welcome to the box\r\nSSH-2.0-OpenSSH_9.6 extra\r\nrest";
    memcpy(s.in, banner, strlen(banner));
    s.in_len = strlen(banner);
    CHECK_EQ(ident_try(&s), PSSH_OK);
    CHECK(strcmp(s.v_s, "SSH-2.0-OpenSSH_9.6 extra") == 0);
    CHECK_EQ(s.in_len, 4); /* "rest" is left for the packet reader */
    memcpy(s.in, "SSH-1.5-old\r\n", 13);
    s.in_len = 13;
    CHECK_EQ(ident_try(&s), PSSH_E_PROTO);
    memcpy(s.in, "SSH-2.0-", 8);
    s.in_len = 8;
    CHECK_EQ(ident_try(&s), PSSH_E_AGAIN);
}

TEST(kexinit_negotiation) {
    static pssh_t s;
    static uint8_t payload[PSSH_KEXINIT_MAX];
    size_t n = 0;
    tnet t;
    memset(&t, 0, sizeof t);
    pssh_ops_t o = tb_ops(&t);
    pssh_init(&s, &o);
    /* aes256 is the first of ours the server has; the server's aes128 is the s2c choice */
    make_kexinit(payload, &n, "diffie-hellman-group14-sha256,curve25519-sha256", "rsa-sha2-256,ssh-ed25519",
                 "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com", "aes128-gcm@openssh.com", 0, 0);
    CHECK_EQ(kexinit_parse(&s, payload, n), PSSH_OK);
    CHECK_EQ(s.cipher_c2s, PSSH_C_GCM256);
    CHECK_EQ(s.cipher_s2c, PSSH_C_GCM128);
    CHECK_EQ(s.hostkey_alg, 1);
    CHECK_EQ(s.strict, 0);
    make_kexinit(payload, &n, "curve25519-sha256", "ecdsa-sha2-nistp256", "chacha20-poly1305@openssh.com",
                 "chacha20-poly1305@openssh.com", 1, 0);
    CHECK_EQ(kexinit_parse(&s, payload, n), PSSH_OK);
    CHECK_EQ(s.hostkey_alg, 4);
    CHECK_EQ(s.strict, 1);
    CHECK_EQ(s.cipher_c2s, PSSH_C_CHACHA);
    make_kexinit(payload, &n, "diffie-hellman-group14-sha256", "ssh-ed25519", "aes128-gcm@openssh.com",
                 "aes128-gcm@openssh.com", 0, 0);
    CHECK_EQ(kexinit_parse(&s, payload, n), PSSH_E_KEX); /* no curve25519: no exchange */
    make_kexinit(payload, &n, "curve25519-sha256", "ssh-ed25519", "aes128-gcm@openssh.com",
                 "aes128-gcm@openssh.com", 0, 1);
    CHECK_EQ(kexinit_parse(&s, payload, n), PSSH_E_KEX); /* a guessed first packet is refused */
    make_kexinit(payload, &n, "curve25519-sha256", "ssh-ed25519", "arcfour,blowfish-cbc", "arcfour", 0, 0);
    CHECK_EQ(kexinit_parse(&s, payload, n), PSSH_E_KEX); /* no common cipher */
}

/* Packets: every cipher, both directions, many sizes, then tampering and a length beyond the limit. */
static void packet_round_trip(int cipher, const char *list) {
    static pssh_t c;
    net_reset();
    memcpy(T.k_srv, vec_K, 32);
    memcpy(T.h_srv, vec_H, 32);
    pssh_ops_t o = tb_ops(&T);
    CHECK_EQ(pssh_init(&c, &o), PSSH_OK);
    CHECK_EQ(pssh_install_keys(&c, cipher, cipher, vec_K, vec_H), PSSH_OK);
    pdir_t from_client = {0}, to_client = {0};
    pdir_t *in = &from_client, *out = &to_client; /* the server: receives the client's keys, sends its own */
    peer_keys(&T, in, cipher, 'A', 'C');
    peer_keys(&T, out, cipher, 'B', 'D');
    (void)list;
    static const size_t sizes[] = {1, 5, 12, 15, 16, 17, 255, 1000, 16384};
    uint8_t pl[16384], got[16384];
    int ok = 1;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        for (size_t i = 0; i < sizes[k]; i++) pl[i] = (uint8_t)(i * 7 + k);
        CHECK_EQ(pssh_send(&c, pl, sizes[k]), PSSH_OK);
        CHECK_EQ(pssh_flush(&c), PSSH_OK);
        /* AES-GCM sends the length in clear: OpenSSH checks that the packet without its length field is 16-aligned */
        if (cipher != PSSH_C_CHACHA) {
            uint32_t plen = ((uint32_t)T.a2b[0] << 24) | ((uint32_t)T.a2b[1] << 16) | ((uint32_t)T.a2b[2] << 8) | T.a2b[3];
            CHECK_EQ(plen % 16, 0);
        }
        const uint8_t *p = 0;
        size_t n = 0;
        int rc = a2b_pop(&T, in, &p, &n);
        if (rc != 0 || n != sizes[k] || memcmp(p, pl, n) != 0) ok = 0;
    }
    CHECK(ok);
    /* server to client, with the client's receive side */
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        for (size_t i = 0; i < sizes[k]; i++) pl[i] = (uint8_t)(i * 11 + k);
        b2a_send(&T, out, pl, sizes[k]);
    }
    CHECK_EQ(io_fill(&c), (int)T.b2a_n);
    ok = 1;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const uint8_t *p = 0;
        size_t n = 0;
        if (pssh_recv_try(&c, &p, &n) != PSSH_OK || n != sizes[k]) { ok = 0; break; }
        for (size_t i = 0; i < n; i++) got[i] = p[i];
        for (size_t i = 0; i < n; i++)
            if (got[i] != (uint8_t)(i * 11 + k)) ok = 0;
    }
    CHECK(ok);
    const uint8_t *p = 0;
    size_t n = 0;
    CHECK_EQ(pssh_recv_try(&c, &p, &n), PSSH_E_AGAIN);

    /* a flipped byte in a packet: the MAC (or the AEAD tag) fails */
    net_reset();
    memcpy(T.k_srv, vec_K, 32);
    memcpy(T.h_srv, vec_H, 32);
    static pssh_t d;
    pssh_ops_t o2 = tb_ops(&T);
    pssh_init(&d, &o2);
    pssh_install_keys(&d, cipher, cipher, vec_K, vec_H);
    pdir_t out2 = {0};
    peer_keys(&T, &out2, cipher, 'B', 'D');
    uint8_t wire[256];
    size_t wn = peer_pack(&T, &out2, (const uint8_t *)"tamper me", 9, wire);
    wire[wn / 2] ^= 0x40;
    tb_put(T.b2a, &T.b2a_n, wire, wn);
    io_fill(&d);
    CHECK_EQ(pssh_recv_try(&d, &p, &n), PSSH_E_PROTO);

    /* a declared length above the limit is refused before any data is read */
    if (cipher == PSSH_C_GCM128 || cipher == PSSH_C_GCM256) {
        net_reset();
    memcpy(T.k_srv, vec_K, 32);
    memcpy(T.h_srv, vec_H, 32);
        pssh_init(&d, &o2);
        pssh_install_keys(&d, cipher, cipher, vec_K, vec_H);
        uint8_t big[8] = {0x00, 0x00, 0x88, 0xb9, 0, 0, 0, 0}; /* 35001 */
        tb_put(T.b2a, &T.b2a_n, big, 8);
        io_fill(&d);
        CHECK_EQ(pssh_recv_try(&d, &p, &n), PSSH_E_PROTO);
    }
}

TEST(packets_aes128_gcm) { packet_round_trip(PSSH_C_GCM128, "aes128-gcm@openssh.com"); }
TEST(packets_aes256_gcm) { packet_round_trip(PSSH_C_GCM256, "aes256-gcm@openssh.com"); }
TEST(packets_chacha20_poly1305) { packet_round_trip(PSSH_C_CHACHA, "chacha20-poly1305@openssh.com"); }

/* A complete handshake against the in-test server, then one message each way on the negotiated keys. */
static void handshake_round(const char *ciphers, int strict) {
    static pssh_t c;
    net_reset();
    host_key_setup();
    T.srv_ciphers = ciphers;
    T.srv_strict = strict;
    pssh_ops_t o = tb_ops(&T);
    CHECK_EQ(pssh_init(&c, &o), PSSH_OK);
    strcpy(c.host, "example.test");
    c.port = 22;
    c.hostkey_check = NULL;
    CHECK_EQ(pssh_handshake(&c, 20000), PSSH_OK);
    CHECK(c.kex_done);
    CHECK_EQ(c.strict, strict);
    CHECK(c.have_sid && memcmp(c.sid, T.h_srv, 32) == 0);
    CHECK_EQ(c.cipher_c2s, expected_cipher(ciphers));
    /* one message each way, on the negotiated keys (the server reads our NEWKEYS when it next runs) */
    server_pump(&T);
    uint8_t p[] = "ping";
    CHECK_EQ(pssh_send(&c, p, 4), PSSH_OK);
    CHECK_EQ(pssh_flush(&c), PSSH_OK);
    const uint8_t *q = 0;
    size_t n = 0;
    CHECK_EQ(a2b_pop(&T, &T.p_in, &q, &n), 0);
    CHECK(n == 4 && memcmp(q, "ping", 4) == 0);
    b2a_send(&T, &T.p_out, (const uint8_t *)"pong", 4);
    int waiting = (int)(T.b2a_n - T.b2a_r);
    CHECK_EQ(io_fill(&c), waiting);
    CHECK_EQ(pssh_recv_try(&c, &q, &n), PSSH_OK);
    CHECK(n == 4 && memcmp(q, "pong", 4) == 0);
}

static int hk_accept(void *ctx, const char *host, uint16_t port, const char *alg, const uint8_t *blob, size_t n) {
    tnet *t = (tnet *)ctx;
    t->hostkey_calls++;
    if (strcmp(host, "example.test") != 0 || port != 22 || strcmp(alg, "ssh-ed25519") != 0) return -1;
    if (n > sizeof t->seen_blob) return -1;
    memcpy(t->seen_blob, blob, n);
    t->seen_n = n;
    return t->refuse ? -1 : 0;
}

TEST(handshake_each_negotiation) {
    handshake_round("aes128-gcm@openssh.com,aes256-gcm@openssh.com,chacha20-poly1305@openssh.com", 0);
    handshake_round("chacha20-poly1305@openssh.com", 0);
    handshake_round("aes256-gcm@openssh.com", 1);
    handshake_round("chacha20-poly1305@openssh.com,aes128-gcm@openssh.com", 1);
}

TEST(handshake_host_key_callback_and_refusals) {
    static pssh_t c;
    net_reset();
    host_key_setup();
    T.srv_ciphers = "aes128-gcm@openssh.com";
    pssh_ops_t o = tb_ops(&T);
    pssh_init(&c, &o);
    strcpy(c.host, "example.test");
    c.port = 22;
    c.hostkey_check = hk_accept;
    c.hostkey_ctx = &T;
    CHECK_EQ(pssh_handshake(&c, 20000), PSSH_OK);
    CHECK_EQ(T.hostkey_calls, 1);
    CHECK(T.seen_n == T.host_blob_n && memcmp(T.seen_blob, T.host_blob, T.host_blob_n) == 0);

    net_reset();
    host_key_setup();
    T.srv_ciphers = "aes128-gcm@openssh.com";
    T.refuse = 1;
    pssh_init(&c, &o);
    strcpy(c.host, "example.test");
    c.port = 22;
    c.hostkey_check = hk_accept;
    c.hostkey_ctx = &T;
    CHECK_EQ(pssh_handshake(&c, 20000), PSSH_E_REFUSED);

    net_reset();
    host_key_setup();
    T.srv_ciphers = "aes128-gcm@openssh.com";
    T.bad_sig = 1;
    pssh_init(&c, &o);
    strcpy(c.host, "example.test");
    c.hostkey_check = NULL;
    CHECK_EQ(pssh_handshake(&c, 20000), PSSH_E_KEX);

    net_reset();
    T.state = -1; /* a server that stops talking: the client gives up at its timeout */
    pssh_init(&c, &o);
    strcpy(c.host, "example.test");
    CHECK(pssh_handshake(&c, 50) != PSSH_OK);
}

/* RSA and ECDSA host keys are verified through the backend; on the host it refuses, and the library says so. */
TEST(hostkey_signature_algorithms) {
    static pssh_t c;
    tnet t;
    memset(&t, 0, sizeof t);
    pssh_ops_t o = tb_ops(&t);
    pssh_init(&c, &o);
    uint8_t h[32];
    memset(h, 0x11, sizeof h);
    uint8_t ecdsa_key[128], ecdsa_sig[128];
    pssh_wb_t kb, sb, inner;
    uint8_t q[65];
    memset(q, 0x22, sizeof q);
    q[0] = 4;
    wb_init(&kb, ecdsa_key, sizeof ecdsa_key);
    wb_cstr(&kb, "ecdsa-sha2-nistp256");
    wb_cstr(&kb, "nistp256");
    wb_str(&kb, q, 65);
    uint8_t rr[32], ss[32];
    memset(rr, 0x55, 32);
    memset(ss, 0x66, 32);
    uint8_t innerb[80];
    wb_init(&inner, innerb, sizeof innerb);
    wb_mpint(&inner, rr, 32);
    wb_mpint(&inner, ss, 32);
    wb_init(&sb, ecdsa_sig, sizeof ecdsa_sig);
    wb_cstr(&sb, "ecdsa-sha2-nistp256");
    wb_str(&sb, innerb, inner.n);
    pssh_bytes_t ks = {ecdsa_key, kb.n}, sig = {ecdsa_sig, sb.n};
    CHECK_EQ(verify_host_sig(&c, ks, sig, h), PSSH_E_UNSUPPORTED);
    /* a host key of another type is refused as a protocol error */
    pssh_wb_t bad;
    uint8_t badkey[32];
    wb_init(&bad, badkey, sizeof badkey);
    wb_cstr(&bad, "ssh-dss");
    pssh_bytes_t bk = {badkey, bad.n};
    CHECK_EQ(verify_host_sig(&c, bk, sig, h), PSSH_E_PROTO);
}

/* The backend is required: a state without one is refused at init. */
TEST(init_requires_a_backend) {
    static pssh_t c;
    CHECK_EQ(pssh_init(&c, NULL), PSSH_E_ARG);
    pssh_ops_t o;
    memset(&o, 0, sizeof o);
    CHECK_EQ(pssh_init(&c, &o), PSSH_E_ARG);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(base64_vectors);
    RUN(wire_writer_and_reader);
    RUN(fingerprint_and_host_blob);
    RUN(exchange_hash_and_key_derivation_vectors);
    RUN(known_hosts_lookup_and_lines);
    RUN(identification_string);
    RUN(kexinit_negotiation);
    RUN(packets_aes128_gcm);
    RUN(packets_aes256_gcm);
    RUN(packets_chacha20_poly1305);
    RUN(handshake_each_negotiation);
    RUN(handshake_host_key_callback_and_refusals);
    RUN(hostkey_signature_algorithms);
    RUN(init_requires_a_backend);
    return TEST_DONE();
}
