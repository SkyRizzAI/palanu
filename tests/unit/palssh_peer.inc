/* The in-memory network, the test backend and the in-test SSH server of test_palssh.c (one translation unit with it,
 * kept apart to stay under the 800-line rule). Nothing here is part of the library. */

/* ---- the in-memory network and the test backend ---- */

typedef struct pdir {
    int cipher;
    uint8_t key[64];
    uint8_t iv[12];
    uint32_t seq;
    int tab; /* index of the AEAD context in tnet.tab */
} pdir_t;

typedef struct tnet {
    uint8_t a2b[T_QUEUE]; /* client -> server bytes not yet read by the server */
    size_t a2b_n;
    uint8_t b2a[T_QUEUE]; /* server -> client bytes not yet read by the client */
    size_t b2a_n, b2a_r;
    int64_t clock;
    uint32_t rnd;
    ci_cipher_t tab[64];
    int tab_n;
    /* the in-test server */
    int state; /* P_* below, -1 after a protocol error */
    pdir_t p_in, p_out;
    uint8_t i_c[PSSH_KEXINIT_MAX], i_s[PSSH_KEXINIT_MAX];
    size_t i_c_n, i_s_n;
    uint8_t sk[64], pk[32];
    uint8_t host_blob[128];
    size_t host_blob_n;
    const char *srv_ciphers;
    int srv_strict;
    int bad_sig;
    uint8_t k_srv[32], h_srv[32];
    int hostkey_calls, refuse;
    uint8_t seen_blob[128];
    size_t seen_n;
} tnet;

enum { P_IDENT, P_KEXINIT, P_ECDH, P_NEWKEYS, P_RUN };

static void tb_put(uint8_t *q, size_t *n, const void *d, size_t k) {
    if (*n + k > T_QUEUE) {
        printf("  queue overflow\n");
        return;
    }
    memcpy(q + *n, d, k);
    *n += k;
}

static int tb_sock_read(void *c, uint8_t *buf, size_t n) {
    tnet *t = (tnet *)c;
    size_t avail = t->b2a_n - t->b2a_r;
    if (avail == 0) return PSSH_E_AGAIN;
    if (n > avail) n = avail;
    memcpy(buf, t->b2a + t->b2a_r, n);
    t->b2a_r += n;
    return (int)n;
}

static int tb_sock_write(void *c, const uint8_t *buf, size_t n) {
    tnet *t = (tnet *)c;
    size_t room = T_QUEUE - t->a2b_n;
    if (room == 0) return PSSH_E_AGAIN;
    if (n > room) n = room;
    memcpy(t->a2b + t->a2b_n, buf, n);
    t->a2b_n += n;
    return (int)n;
}

static void server_pump(tnet *t);

static int tb_wait(void *c, unsigned want, int timeout_ms, unsigned *got) {
    tnet *t = (tnet *)c;
    (void)timeout_ms;
    server_pump(t); /* the server answers what the client sent so far */
    *got = 0;
    if ((want & PSSH_W_SOCK_IN) && t->b2a_n > t->b2a_r) *got |= PSSH_W_SOCK_IN;
    if (want & PSSH_W_SOCK_OUT) *got |= PSSH_W_SOCK_OUT;
    return 0;
}

static int tb_event(void *c, int *kind, int *cols, int *rows) {
    (void)c;
    (void)cols;
    (void)rows;
    *kind = PSSH_EV_NONE;
    return 0;
}

static int tb_size(void *c, int *cols, int *rows) {
    (void)c;
    *cols = 80;
    *rows = 24;
    return 0;
}

static int64_t tb_now(void *c) {
    tnet *t = (tnet *)c;
    return t->clock++; /* every call moves the clock: a silent peer times out instead of hanging */
}

static int tb_random(void *c, uint8_t *buf, size_t n) {
    tnet *t = (tnet *)c;
    for (size_t i = 0; i < n; i++) {
        t->rnd = t->rnd * 1103515245u + 12345u;
        buf[i] = (uint8_t)(t->rnd >> 16);
    }
    return 0;
}

static int tb_stdin(void *c, uint8_t *buf, size_t n) {
    (void)c;
    (void)buf;
    (void)n;
    return 0;
}

static int tb_stdout(void *c, int fd, const uint8_t *buf, size_t n) {
    (void)c;
    (void)fd;
    (void)buf;
    return (int)n;
}

static int tb_hash(void *c, int alg, const uint8_t *d, size_t n, uint8_t *out) {
    (void)c;
    crypto_mac_t m;
    if (crypto_mac_hash(&m, alg) != NEMA_OK) return PSSH_E_UNSUPPORTED;
    crypto_mac_update(&m, d, n);
    crypto_mac_final(&m, out);
    return 0;
}

static int tb_hmac(void *c, int alg, const uint8_t *key, size_t klen, const uint8_t *d, size_t n, uint8_t *out) {
    (void)c;
    crypto_mac_t m;
    if (crypto_mac_hmac(&m, alg, key, klen) != NEMA_OK) return PSSH_E_UNSUPPORTED;
    crypto_mac_update(&m, d, n);
    crypto_mac_final(&m, out);
    return 0;
}

static int tb_x25519(void *c, uint8_t out[32], const uint8_t scalar[32], const uint8_t *point) {
    (void)c;
    if (!point) {
        prim_x25519_base(out, scalar);
        return 0;
    }
    return prim_x25519(out, scalar, point) == 0 ? 0 : PSSH_E_KEX;
}

static int tb_ed25519(void *c, const uint8_t pk[32], const uint8_t *msg, size_t n, const uint8_t sig[64]) {
    (void)c;
    return prim_ed25519_verify(pk, msg, n, sig) == 0 ? 0 : PSSH_E_KEX;
}

/* ECDSA and RSA are not on the host: the primitive refuses, and the library reports it as unsupported. */
static int tb_ecdsa(void *c, const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]) {
    (void)c;
    return ci_ecdsa_p256_verify(pub, hash, 32, sig) == 0 ? 0 : PSSH_E_UNSUPPORTED;
}

static int tb_rsa(void *c, const uint8_t *n, size_t nl, const uint8_t *e, size_t el, int halg, const uint8_t *hash,
                  size_t hl, const uint8_t *sig, size_t sl) {
    (void)c;
    return ci_rsa_verify_pkcs1(n, nl, e, el, halg, hash, hl, sig, sl) == 0 ? 0 : PSSH_E_UNSUPPORTED;
}

static int tb_chacha(void *c, const uint8_t key[32], const uint8_t nonce[8], uint64_t counter, const uint8_t *in,
                     uint8_t *out, size_t n) {
    (void)c;
    return prim_chacha20_xor(key, nonce, 8, counter, in, out, n) == 0 ? 0 : PSSH_E_IO;
}

static int tb_poly(void *c, const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t tag[16]) {
    (void)c;
    prim_poly1305(key, msg, n, tag);
    return 0;
}

static int tb_aead_open(void *c, int alg, const uint8_t *key, size_t klen) {
    tnet *t = (tnet *)c;
    (void)alg;
    if (t->tab_n >= 64) return PSSH_E_IO;
    if (ci_gcm_init(&t->tab[t->tab_n], key, klen) != NEMA_OK) return PSSH_E_IO;
    return t->tab_n++;
}

static int tb_aead_seal(void *c, int h, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                        size_t n, uint8_t tag[16]) {
    tnet *t = (tnet *)c;
    return ci_gcm_seal(&t->tab[h], nonce, aad, alen, buf, n, tag) == NEMA_OK ? 0 : PSSH_E_IO;
}

static int tb_aead_dec(void *c, int h, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                       size_t n, const uint8_t tag[16]) {
    tnet *t = (tnet *)c;
    return ci_gcm_open(&t->tab[h], nonce, aad, alen, buf, n, tag) == NEMA_OK ? 0 : PSSH_E_AUTH;
}

/* The keystore is the device's: the host tests have no keys. */
static int tb_key_load(void *c, const char *name, size_t nlen) {
    (void)c;
    (void)name;
    (void)nlen;
    return PSSH_E_NOKEY;
}

static int tb_key_public(void *c, int h, uint8_t *out, size_t cap) {
    (void)c;
    (void)h;
    (void)out;
    (void)cap;
    return -1;
}

static int tb_key_sign(void *c, int h, const uint8_t *d, size_t n, uint8_t *sig, size_t cap) {
    (void)c;
    (void)h;
    (void)d;
    (void)n;
    (void)sig;
    (void)cap;
    return -1;
}

static int tb_secret(void *c, const char *prompt, char *buf, size_t cap) {
    (void)c;
    (void)prompt;
    (void)cap;
    memcpy(buf, "hunter2", 7);
    return 7;
}

static pssh_ops_t tb_ops(tnet *t) {
    pssh_ops_t o;
    memset(&o, 0, sizeof o);
    o.ctx = t;
    o.sock_read = tb_sock_read;
    o.sock_write = tb_sock_write;
    o.stdin_read = tb_stdin;
    o.stdout_write = tb_stdout;
    o.wait = tb_wait;
    o.event = tb_event;
    o.size = tb_size;
    o.now_ms = tb_now;
    o.random = tb_random;
    o.hash = tb_hash;
    o.hmac = tb_hmac;
    o.x25519 = tb_x25519;
    o.ed25519_verify = tb_ed25519;
    o.ecdsa_p256_verify = tb_ecdsa;
    o.rsa_verify = tb_rsa;
    o.chacha20 = tb_chacha;
    o.poly1305 = tb_poly;
    o.aead_open = tb_aead_open;
    o.aead_seal = tb_aead_seal;
    o.aead_decrypt = tb_aead_dec;
    o.key_load = tb_key_load;
    o.key_public = tb_key_public;
    o.key_sign = tb_key_sign;
    o.secret = tb_secret;
    return o;
}

/* ---- the peer side of the packet layer, written again from RFC 4253 section 6 and OpenSSH's chacha20 notes ---- */

static void be32_put(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)(v >> 24);
    d[1] = (uint8_t)(v >> 16);
    d[2] = (uint8_t)(v >> 8);
    d[3] = (uint8_t)v;
}

static void be64_put(uint8_t *d, uint64_t v) {
    be32_put(d, (uint32_t)(v >> 32));
    be32_put(d + 4, (uint32_t)v);
}

static void iv_bump(uint8_t iv[12]) {
    for (int i = 11; i >= 4; i--)
        if (++iv[i]) break;
}

/* Packs one payload for direction d into o. Returns the bytes written. */
static size_t peer_pack(tnet *t, pdir_t *d, const uint8_t *pl, size_t n, uint8_t *o) {
    /* the AEAD ciphers align padding_length || payload || padding (the length field is not counted); none aligns all */
    int aead = d->cipher != PSSH_C_NONE;
    size_t block = (d->cipher == PSSH_C_GCM128 || d->cipher == PSSH_C_GCM256) ? 16 : 8;
    size_t pad = block - ((aead ? 1 : 5) + n) % block;
    if (pad < 4) pad += block;
    size_t plen = 1 + n + pad;
    be32_put(o, (uint32_t)plen);
    o[4] = (uint8_t)pad;
    memcpy(o + 5, pl, n);
    memset(o + 5 + n, 0x5a, pad); /* padding is any bytes: the receiver must not look at them */
    size_t total = 4 + plen;
    if (d->cipher == PSSH_C_CHACHA) {
        uint8_t nonce[8], zero[32] = {0}, pk[32], ks[4], lb[4];
        be64_put(nonce, d->seq);
        uint8_t *k2 = d->key, *k1 = d->key + 32; /* main key first, header key second */
        be32_put(lb, (uint32_t)plen);
        prim_chacha20_xor(k1, nonce, 8, 0, zero, ks, 4);
        for (int i = 0; i < 4; i++) o[i] = lb[i] ^ ks[i];
        prim_chacha20_xor(k2, nonce, 8, 1, o + 4, o + 4, plen);
        prim_chacha20_xor(k2, nonce, 8, 0, zero, pk, 32);
        prim_poly1305(pk, o, 4 + plen, o + total);
        total += 16;
    } else if (d->cipher != PSSH_C_NONE) {
        ci_gcm_seal(&t->tab[d->tab], d->iv, o, 4, o + 4, plen, o + total);
        iv_bump(d->iv);
        total += 16;
    }
    d->seq++;
    return total;
}

/* Unpacks the next packet in place. Returns 0 with the payload, 1 when incomplete, -1 when it is bad. */
static int peer_unpack(tnet *t, pdir_t *d, uint8_t *in, size_t avail, const uint8_t **pl, size_t *n, size_t *used) {
    if (avail < 4) return 1;
    size_t tag = d->cipher == PSSH_C_NONE ? 0 : 16;
    uint32_t plen;
    if (d->cipher == PSSH_C_CHACHA) {
        uint8_t nonce[8], zero[4] = {0}, ks[4];
        be64_put(nonce, d->seq);
        prim_chacha20_xor(d->key + 32, nonce, 8, 0, zero, ks, 4);
        plen = ((uint32_t)(in[0] ^ ks[0]) << 24) | ((uint32_t)(in[1] ^ ks[1]) << 16) |
               ((uint32_t)(in[2] ^ ks[2]) << 8) | (uint32_t)(in[3] ^ ks[3]);
    } else {
        plen = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | in[3];
    }
    if (plen < 5 || plen > 35000) return -1;
    /* the alignment a server checks (OpenSSH: "padding error"): not for a packet with no cipher, which counts the length */
    if (d->cipher == PSSH_C_NONE && (4 + (size_t)plen) % 8) return -1;
    if (d->cipher != PSSH_C_NONE && plen % (d->cipher == PSSH_C_CHACHA ? 8 : 16)) return -1;
    if (avail < 4 + (size_t)plen + tag) return 1;
    if (d->cipher == PSSH_C_CHACHA) {
        uint8_t nonce[8], zero[32] = {0}, pk[32], mac[16];
        be64_put(nonce, d->seq);
        prim_chacha20_xor(d->key, nonce, 8, 0, zero, pk, 32);
        prim_poly1305(pk, in, 4 + plen, mac);
        if (memcmp(mac, in + 4 + plen, 16) != 0) return -1;
        prim_chacha20_xor(d->key, nonce, 8, 1, in + 4, in + 4, plen);
    } else if (d->cipher != PSSH_C_NONE) {
        if (ci_gcm_open(&t->tab[d->tab], d->iv, in, 4, in + 4, plen, in + 4 + plen) != NEMA_OK) return -1;
        iv_bump(d->iv);
    }
    uint8_t padlen = in[4];
    if (padlen < 4 || padlen >= plen) return -1;
    *pl = in + 5;
    *n = plen - 1 - padlen;
    *used = 4 + plen + tag;
    d->seq++;
    return 0;
}

/* The server reads one packet from the client's stream and drops its bytes. */
static int a2b_pop(tnet *t, pdir_t *d, const uint8_t **pl, size_t *n) {
    size_t used = 0;
    int rc = peer_unpack(t, d, t->a2b, t->a2b_n, pl, n, &used);
    if (rc != 0) return rc;
    memmove(t->a2b, t->a2b + used, t->a2b_n - used);
    t->a2b_n -= used;
    return 0;
}

static void b2a_send(tnet *t, pdir_t *d, const uint8_t *pl, size_t n) {
    static uint8_t buf[PSSH_BUF + 64]; /* one packet at most: the largest test payload is 16384 */
    size_t k = peer_pack(t, d, pl, n, buf);
    tb_put(t->b2a, &t->b2a_n, buf, k);
}

/* The peer's key setup: the same derivation as the client (RFC 4253 section 7.2), its own AEAD context. */
static void peer_keys(tnet *t, pdir_t *d, int cipher, char ivl, char keyl) {
    pssh_ops_t o = tb_ops(t);
    d->cipher = cipher;
    if (cipher == PSSH_C_CHACHA) {
        pssh_derive(&o, t->k_srv, t->h_srv, t->h_srv, keyl, d->key, 64);
        return;
    }
    size_t klen = cipher == PSSH_C_GCM128 ? 16 : 32;
    pssh_derive(&o, t->k_srv, t->h_srv, t->h_srv, keyl, d->key, klen);
    pssh_derive(&o, t->k_srv, t->h_srv, t->h_srv, ivl, d->iv, 12);
    d->tab = tb_aead_open(t, 0, d->key, klen);
}

/* The first cipher of our list that the server also offers (the rule of the client). */
static int expected_cipher(const char *list) {
    if (strstr(list, "aes128-gcm@openssh.com")) return PSSH_C_GCM128;
    if (strstr(list, "aes256-gcm@openssh.com")) return PSSH_C_GCM256;
    if (strstr(list, "chacha20-poly1305@openssh.com")) return PSSH_C_CHACHA;
    return PSSH_C_NONE;
}

/* Builds a KEXINIT payload (RFC 4253 section 7.1). */
static void make_kexinit(uint8_t *out, size_t *outn, const char *kex, const char *hk, const char *c2s,
                         const char *s2c, int strict, int first) {
    pssh_wb_t b;
    wb_init(&b, out, PSSH_KEXINIT_MAX);
    wb_u8(&b, SSH_MSG_KEXINIT);
    for (int i = 0; i < 16; i++) wb_u8(&b, (uint8_t)(0xc0 + i));
    char kexl[200];
    snprintf(kexl, sizeof kexl, "%s%s", kex, strict ? ",kex-strict-s-v00@openssh.com" : "");
    wb_cstr(&b, kexl);
    wb_cstr(&b, hk);
    wb_cstr(&b, c2s);
    wb_cstr(&b, s2c);
    wb_cstr(&b, "hmac-sha2-256");
    wb_cstr(&b, "hmac-sha2-256");
    wb_cstr(&b, "none");
    wb_cstr(&b, "none");
    wb_cstr(&b, "");
    wb_cstr(&b, "");
    wb_u8(&b, (uint8_t)first);
    wb_u32(&b, 0);
    *outn = b.n;
}

/* One step of the in-test server, run whenever the client waits. */
static void server_pump(tnet *t) {
    const uint8_t *pl = 0;
    size_t n = 0;
    for (;;) {
        if (t->state == P_IDENT) {
            size_t i = 0;
            while (i < t->a2b_n && t->a2b[i] != '\n') i++;
            if (i == t->a2b_n) return;
            memmove(t->a2b, t->a2b + i + 1, t->a2b_n - i - 1);
            t->a2b_n -= i + 1;
            tb_put(t->b2a, &t->b2a_n, "SSH-2.0-TestServer_1.0\r\n", 24);
            make_kexinit(t->i_s, &t->i_s_n, "curve25519-sha256", "ssh-ed25519", t->srv_ciphers, t->srv_ciphers,
                         t->srv_strict, 0);
            b2a_send(t, &t->p_out, t->i_s, t->i_s_n);
            t->state = P_KEXINIT;
            continue;
        }
        if (t->state == P_KEXINIT) {
            if (a2b_pop(t, &t->p_in, &pl, &n) != 0) return;
            if (n < 1 || n > sizeof t->i_c || pl[0] != SSH_MSG_KEXINIT) {
                t->state = -1;
                return;
            }
            memcpy(t->i_c, pl, n);
            t->i_c_n = n;
            t->state = P_ECDH;
            continue;
        }
        if (t->state == P_ECDH) {
            if (a2b_pop(t, &t->p_in, &pl, &n) != 0) return;
            if (n != 37 || pl[0] != SSH_MSG_KEX_ECDH_INIT) {
                t->state = -1;
                return;
            }
            uint8_t qc[32], eph[32], qs[32], sig[64], sigblob[128], reply[256];
            memcpy(qc, pl + 5, 32);
            tb_random(t, eph, 32);
            prim_x25519_base(qs, eph);
            prim_x25519(t->k_srv, eph, qc);
            pssh_bytes_t parts[7];
            parts[0].p = (const uint8_t *)PSSH_VERSION_LINE;
            parts[0].n = pssh_len(PSSH_VERSION_LINE);
            parts[1].p = (const uint8_t *)"SSH-2.0-TestServer_1.0";
            parts[1].n = 22;
            parts[2].p = t->i_c;
            parts[2].n = t->i_c_n;
            parts[3].p = t->i_s;
            parts[3].n = t->i_s_n;
            parts[4].p = t->host_blob;
            parts[4].n = t->host_blob_n;
            parts[5].p = qc;
            parts[5].n = 32;
            parts[6].p = qs;
            parts[6].n = 32;
            pssh_ops_t o = tb_ops(t);
            pssh_exchange_hash(&o, parts, t->k_srv, t->h_srv);
            prim_ed25519_sign(sig, t->sk, t->h_srv, 32);
            if (t->bad_sig) sig[5] ^= 1;
            pssh_wb_t sb, rb;
            wb_init(&sb, sigblob, sizeof sigblob);
            wb_cstr(&sb, "ssh-ed25519");
            wb_str(&sb, sig, 64);
            wb_init(&rb, reply, sizeof reply);
            wb_u8(&rb, SSH_MSG_KEX_ECDH_REPLY);
            wb_str(&rb, t->host_blob, t->host_blob_n);
            wb_str(&rb, qs, 32);
            wb_str(&rb, sigblob, sb.n);
            b2a_send(t, &t->p_out, reply, rb.n);
            uint8_t nk = SSH_MSG_NEWKEYS;
            b2a_send(t, &t->p_out, &nk, 1);
            if (t->srv_strict) t->p_out.seq = 0;
            /* the server sends encrypted from here: server-to-client keys (letters B, D) */
            peer_keys(t, &t->p_out, expected_cipher(t->srv_ciphers), 'B', 'D');
            t->state = P_NEWKEYS;
            continue;
        }
        if (t->state == P_NEWKEYS) {
            if (a2b_pop(t, &t->p_in, &pl, &n) != 0) return;
            if (n != 1 || pl[0] != SSH_MSG_NEWKEYS) {
                t->state = -1;
                return;
            }
            if (t->srv_strict) t->p_in.seq = 0;
            /* client-to-server keys (letters A, C) */
            peer_keys(t, &t->p_in, expected_cipher(t->srv_ciphers), 'A', 'C');
            t->state = P_RUN;
            continue;
        }
        return;
    }
}

