/* palssh_transport.inc: identification, KEXINIT negotiation, curve25519-sha256 key exchange, host key signature checks,
 * the three packet protections (aes128/256-gcm@openssh.com, chacha20-poly1305@openssh.com) and the non-blocking I/O
 * that feeds them. RFC 4253 sections 4 to 7, RFC 5647 (AES-GCM), OpenSSH PROTOCOL.chacha20poly1305. */

#define PSSH_KEX_LIST "curve25519-sha256,curve25519-sha256@libssh.org,ext-info-c,kex-strict-c-v00@openssh.com"
#define PSSH_HOSTKEY_LIST "ssh-ed25519,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp256"
#define PSSH_CIPHER_LIST "aes128-gcm@openssh.com,aes256-gcm@openssh.com,chacha20-poly1305@openssh.com"
#define PSSH_MAC_LIST "hmac-sha2-256"
#define PSSH_COMP_LIST "none"
#define PSSH_HS_KEXINIT 0 /* handshake states */
#define PSSH_HS_REPLY 1
#define PSSH_HS_NEWKEYS 2

static int name_is(const char *p, size_t n, const char *s) {
    size_t m = pssh_len(s);
    return n == m && pssh_eq(p, s, n);
}

/* Is the token (tok, tlen) one of the comma-separated names of a name-list field? */
static int nl_has(pssh_bytes_t list, const char *tok, size_t tlen) {
    size_t i = 0;
    while (i < list.n) {
        size_t j = i;
        while (j < list.n && list.p[j] != ',') j++;
        if (j - i == tlen && pssh_eq(list.p + i, tok, tlen)) return 1;
        i = j + 1;
    }
    return 0;
}

/* First name of ours that the peer's list also has (ours sets the order). */
static int nl_pick(const char *ours, pssh_bytes_t theirs, const char **name, size_t *nlen) {
    size_t olen = pssh_len(ours), i = 0;
    while (i < olen) {
        size_t j = i;
        while (j < olen && ours[j] != ',') j++;
        if (nl_has(theirs, ours + i, j - i)) {
            *name = ours + i;
            *nlen = j - i;
            return 1;
        }
        i = j + 1;
    }
    return 0;
}

static int cipher_of(const char *p, size_t n) {
    if (name_is(p, n, "aes128-gcm@openssh.com")) return PSSH_C_GCM128;
    if (name_is(p, n, "aes256-gcm@openssh.com")) return PSSH_C_GCM256;
    if (name_is(p, n, "chacha20-poly1305@openssh.com")) return PSSH_C_CHACHA;
    return PSSH_C_NONE;
}

static int hostkey_of(const char *p, size_t n) {
    if (name_is(p, n, "ssh-ed25519")) return 1;
    if (name_is(p, n, "rsa-sha2-256")) return 2;
    if (name_is(p, n, "rsa-sha2-512")) return 3;
    if (name_is(p, n, "ecdsa-sha2-nistp256")) return 4;
    return 0;
}

/* ---- I/O on the non-blocking socket ---- */
static void in_drop(pssh_t *s, size_t k) {
    if (k > s->in_len) k = s->in_len;
    pssh_copy(s->in, s->in + k, s->in_len - k);
    s->in_len -= k;
}

/* Reads what the socket has into the input buffer. Returns the bytes read, PSSH_E_AGAIN, PSSH_E_EOF or PSSH_E_IO. */
static int io_fill(pssh_t *s) {
    if (s->in_len >= PSSH_BUF) return PSSH_E_PROTO;
    int r = s->ops.sock_read(s->ops.ctx, s->in + s->in_len, PSSH_BUF - s->in_len);
    if (r == PSSH_E_AGAIN) return PSSH_E_AGAIN;
    if (r == 0) return PSSH_E_EOF;
    if (r < 0) return PSSH_E_IO;
    s->in_len += (size_t)r;
    return r;
}

int pssh_flush(pssh_t *s) {
    while (s->out_off < s->out_len) {
        int r = s->ops.sock_write(s->ops.ctx, s->out + s->out_off, s->out_len - s->out_off);
        if (r == PSSH_E_AGAIN || r == 0) return PSSH_E_AGAIN;
        if (r < 0) return PSSH_E_IO;
        s->out_off += (size_t)r;
    }
    s->out_len = s->out_off = 0;
    return PSSH_OK;
}

/* Takes one event from the backend (quit or resize) into the state. */
static void pump_events(pssh_t *s) {
    int kind = PSSH_EV_NONE, cols = 0, rows = 0;
    while (s->ops.event(s->ops.ctx, &kind, &cols, &rows) == 0 && kind != PSSH_EV_NONE) {
        if (kind == PSSH_EV_QUIT) s->quit = 1;
        if (kind == PSSH_EV_RESIZE) {
            s->cols = cols;
            s->rows = rows;
            s->resized = 1;
        }
    }
}

/* Waits for socket and event readiness; the event queue is always in the wait. Returns PSSH_E_QUIT when stopped. */
static int wait_io(pssh_t *s, unsigned want, int timeout_ms, unsigned *got) {
    int rc = s->ops.wait(s->ops.ctx, want | PSSH_W_EVENT, timeout_ms, got);
    if (rc < 0) return PSSH_E_IO;
    if (*got & PSSH_W_EVENT) pump_events(s);
    if (s->quit) return PSSH_E_QUIT;
    return PSSH_OK;
}

/* ---- packet encoding (RFC 4253 section 6, the three protections) ---- */
static void be32(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)(v >> 24);
    d[1] = (uint8_t)(v >> 16);
    d[2] = (uint8_t)(v >> 8);
    d[3] = (uint8_t)v;
}

static void be64(uint8_t *d, uint64_t v) {
    be32(d, (uint32_t)(v >> 32));
    be32(d + 4, (uint32_t)v);
}

static void inc_iv(uint8_t iv[12]) { /* the 8-byte invocation counter at the end, big-endian (OpenSSH) */
    for (int i = 11; i >= 4; i--) {
        if (++iv[i] != 0) break;
    }
}

/* Encodes one payload into the output buffer. Returns PSSH_E_AGAIN when the buffer has no room for it. */
static int pkt_encode(pssh_t *s, const uint8_t *payload, size_t n) {
    pssh_dir_t *d = &s->out_dir;
    if (n == 0 || n > PSSH_MAX_PAYLOAD - 64) return PSSH_E_ARG;
    /* OpenSSH aligns the AEAD ciphers (AES-GCM and chacha20-poly1305@openssh.com) over padding_length || payload ||
     * padding only: the packet length field is not counted. Without a cipher the whole packet, the length included,
     * is aligned (RFC 4253 section 6). The servers check exactly this, so both forms are required. */
    int aead = d->kind != PSSH_C_NONE;
    size_t block = (d->kind == PSSH_C_GCM128 || d->kind == PSSH_C_GCM256) ? 16 : 8;
    size_t pad = block - ((aead ? 1 : 5) + n) % block;
    if (pad < 4) pad += block;
    size_t plen = 1 + n + pad;
    size_t total = 4 + plen + (d->kind == PSSH_C_NONE ? 0 : 16);
    if (s->out_len + total > PSSH_BUF) return PSSH_E_AGAIN;
    uint8_t *o = s->out + s->out_len;
    be32(o, (uint32_t)plen);
    o[4] = (uint8_t)pad;
    pssh_copy(o + 5, payload, n);
    if (s->ops.random(s->ops.ctx, o + 5 + n, pad) != 0) return PSSH_E_IO;
    if (d->kind == PSSH_C_CHACHA) {
        uint8_t nonce[8], zero[32] = {0}, pk[32];
        be64(nonce, d->seq);
        const uint8_t *k2 = d->key, *k1 = d->key + 32; /* main key first, header key second */
        if (s->ops.chacha20(s->ops.ctx, k1, nonce, 0, o, o, 4) != 0) return PSSH_E_IO;
        if (s->ops.chacha20(s->ops.ctx, k2, nonce, 1, o + 4, o + 4, plen) != 0) return PSSH_E_IO;
        if (s->ops.chacha20(s->ops.ctx, k2, nonce, 0, zero, pk, 32) != 0) return PSSH_E_IO;
        if (s->ops.poly1305(s->ops.ctx, pk, o, 4 + plen, o + 4 + plen) != 0) return PSSH_E_IO;
    } else if (d->kind != PSSH_C_NONE) {
        if (s->ops.aead_seal(s->ops.ctx, d->h, d->iv, o, 4, o + 4, plen, o + 4 + plen) != 0) return PSSH_E_IO;
        inc_iv(d->iv);
    }
    d->seq++;
    s->out_len += total;
    return PSSH_OK;
}

/* Decodes the next packet of the input buffer. On success *payload points into s->in and in_used is set; the bytes
 * are dropped by the next call. PSSH_E_AGAIN when the packet is not complete, PSSH_E_PROTO on a bad packet. */
static int pkt_decode(pssh_t *s, const uint8_t **payload, size_t *n) {
    pssh_dir_t *d = &s->in_dir;
    if (s->in_len < 4) return PSSH_E_AGAIN;
    uint8_t *i = s->in;
    uint32_t plen;
    size_t tag = d->kind == PSSH_C_NONE ? 0 : 16;
    if (d->kind == PSSH_C_CHACHA) {
        uint8_t nonce[8], lb[4];
        be64(nonce, d->seq);
        if (s->ops.chacha20(s->ops.ctx, d->key + 32, nonce, 0, i, lb, 4) != 0) return PSSH_E_IO;
        plen = ((uint32_t)lb[0] << 24) | ((uint32_t)lb[1] << 16) | ((uint32_t)lb[2] << 8) | lb[3];
    } else {
        plen = ((uint32_t)i[0] << 24) | ((uint32_t)i[1] << 16) | ((uint32_t)i[2] << 8) | i[3];
    }
    if (plen < 5 || plen > PSSH_MAX_PAYLOAD) return PSSH_E_PROTO;
    size_t total = 4 + (size_t)plen + tag;
    if (s->in_len < total) return PSSH_E_AGAIN;
    if (d->kind == PSSH_C_CHACHA) {
        uint8_t nonce[8], zero[32] = {0}, pk[32], mac[16];
        be64(nonce, d->seq);
        const uint8_t *k2 = d->key;
        if (s->ops.chacha20(s->ops.ctx, k2, nonce, 0, zero, pk, 32) != 0) return PSSH_E_IO;
        if (s->ops.poly1305(s->ops.ctx, pk, i, 4 + (size_t)plen, mac) != 0) return PSSH_E_IO;
        if (!pssh_eq(mac, i + 4 + plen, 16)) return PSSH_E_PROTO;
        if (s->ops.chacha20(s->ops.ctx, k2, nonce, 1, i + 4, i + 4, plen) != 0) return PSSH_E_IO;
    } else if (tag) {
        if (s->ops.aead_decrypt(s->ops.ctx, d->h, d->iv, i, 4, i + 4, plen, i + 4 + plen) != 0) return PSSH_E_PROTO;
        inc_iv(d->iv);
    }
    uint8_t padlen = i[4];
    if (padlen < 4 || padlen >= plen) return PSSH_E_PROTO;
    *payload = i + 5;
    *n = plen - 1 - padlen;
    s->in_used = total;
    d->seq++;
    return PSSH_OK;
}

int pssh_send(pssh_t *s, const uint8_t *payload, size_t n) {
    for (int tries = 0; tries < 64; tries++) {
        int rc = pkt_encode(s, payload, n);
        if (rc != PSSH_E_AGAIN) return rc;
        rc = pssh_flush(s);
        if (rc == PSSH_E_IO) return rc;
        if (rc == PSSH_E_AGAIN) {
            unsigned got = 0;
            rc = wait_io(s, PSSH_W_SOCK_OUT, 1000, &got);
            if (rc == PSSH_E_QUIT) return rc;
        }
    }
    return PSSH_E_IO;
}

int pssh_recv_try(pssh_t *s, const uint8_t **payload, size_t *n) {
    if (s->in_used) {
        in_drop(s, s->in_used);
        s->in_used = 0;
    }
    return pkt_decode(s, payload, n);
}

/* ---- key derivation and exchange hash (RFC 4253 sections 7.2 and 8) ---- */
int pssh_exchange_hash(const pssh_ops_t *ops, const pssh_bytes_t parts[7], const uint8_t k[32], uint8_t out[32]) {
    uint8_t buf[PSSH_HBUF];
    pssh_wb_t b;
    wb_init(&b, buf, sizeof buf);
    for (int i = 0; i < 7; i++) wb_str(&b, parts[i].p, parts[i].n);
    wb_mpint(&b, k, 32);
    if (b.err) return PSSH_E_ARG;
    return ops->hash(ops->ctx, PSSH_H_SHA256, buf, b.n, out) == 0 ? PSSH_OK : PSSH_E_UNSUPPORTED;
}

int pssh_derive(const pssh_ops_t *ops, const uint8_t k[32], const uint8_t h[32], const uint8_t sid[32], char letter,
                uint8_t *out, size_t n) {
    uint8_t in[256];
    uint8_t blocks[128];
    if (n == 0 || n > sizeof blocks) return PSSH_E_ARG;
    pssh_wb_t b;
    wb_init(&b, in, sizeof in);
    wb_mpint(&b, k, 32);
    wb_raw(&b, h, 32);
    size_t base = b.n, got = 0;
    while (got < n) {
        b.n = base;
        if (got == 0) {
            wb_u8(&b, (uint8_t)letter);
            wb_raw(&b, sid, 32);
        } else {
            wb_raw(&b, blocks, got);
        }
        if (b.err) return PSSH_E_ARG;
        if (ops->hash(ops->ctx, PSSH_H_SHA256, in, b.n, blocks + got) != 0) return PSSH_E_UNSUPPORTED;
        got += 32;
    }
    pssh_copy(out, blocks, n);
    pssh_zero(blocks, sizeof blocks);
    return PSSH_OK;
}

/* Sets one direction up for cipher: the key and the IV are derived with the letters keyl and ivl. The sequence number
 * is kept: it counts every packet of the connection. */
static int setup_dir(pssh_t *s, pssh_dir_t *d, int cipher, const uint8_t k[32], const uint8_t h[32], char ivl,
                     char keyl) {
    const pssh_ops_t *o = &s->ops;
    if (cipher == PSSH_C_NONE) return PSSH_E_KEX;
    if (cipher == PSSH_C_CHACHA) {
        if (pssh_derive(o, k, h, s->sid, keyl, d->key, 64) != 0) return PSSH_E_KEX;
        d->kind = PSSH_C_CHACHA;
        return PSSH_OK;
    }
    size_t klen = cipher == PSSH_C_GCM128 ? 16 : 32;
    if (pssh_derive(o, k, h, s->sid, keyl, d->key, klen) != 0) return PSSH_E_KEX;
    if (pssh_derive(o, k, h, s->sid, ivl, d->iv, 12) != 0) return PSSH_E_KEX;
    int hd = o->aead_open(o->ctx, cipher == PSSH_C_GCM128 ? 1 : 2, d->key, klen);
    if (hd < 0) return PSSH_E_IO;
    d->h = hd;
    d->kind = cipher;
    return PSSH_OK;
}

int pssh_install_keys(pssh_t *s, int cipher_c2s, int cipher_s2c, const uint8_t k[32], const uint8_t h[32]) {
    if (!s->have_sid) {
        pssh_copy(s->sid, h, 32);
        s->have_sid = 1;
    }
    int rc = setup_dir(s, &s->out_dir, cipher_c2s, k, h, 'A', 'C');
    if (rc == PSSH_OK) rc = setup_dir(s, &s->in_dir, cipher_s2c, k, h, 'B', 'D');
    return rc;
}

/* ---- KEXINIT (RFC 4253 section 7.1) ---- */
static int kexinit_build(pssh_t *s) {
    pssh_wb_t b;
    wb_init(&b, s->i_c, sizeof s->i_c);
    wb_u8(&b, SSH_MSG_KEXINIT);
    uint8_t cookie[16];
    if (s->ops.random(s->ops.ctx, cookie, 16) != 0) return PSSH_E_IO;
    wb_raw(&b, cookie, 16);
    wb_cstr(&b, PSSH_KEX_LIST);
    wb_cstr(&b, PSSH_HOSTKEY_LIST);
    wb_cstr(&b, PSSH_CIPHER_LIST);
    wb_cstr(&b, PSSH_CIPHER_LIST);
    wb_cstr(&b, PSSH_MAC_LIST);
    wb_cstr(&b, PSSH_MAC_LIST);
    wb_cstr(&b, PSSH_COMP_LIST);
    wb_cstr(&b, PSSH_COMP_LIST);
    wb_cstr(&b, "");
    wb_cstr(&b, "");
    wb_u8(&b, 0);
    wb_u32(&b, 0);
    if (b.err) return PSSH_E_ARG;
    s->i_c_len = b.n;
    return PSSH_OK;
}

static int kexinit_parse(pssh_t *s, const uint8_t *p, size_t n) {
    if (n > PSSH_KEXINIT_MAX || n < 17) return PSSH_E_PROTO;
    if (p[0] != SSH_MSG_KEXINIT) return PSSH_E_PROTO;
    pssh_copy(s->i_s, p, n);
    s->i_s_len = n;
    pssh_rb_t r;
    rb_init(&r, p + 1 + 16, n - 17); /* skip the message number and the cookie */
    pssh_bytes_t kex = rb_str(&r), hk = rb_str(&r), c2s = rb_str(&r), s2c = rb_str(&r);
    rb_str(&r);
    rb_str(&r); /* mac c2s, mac s2c */
    rb_str(&r);
    rb_str(&r); /* compression */
    rb_str(&r);
    rb_str(&r); /* languages */
    int first_kex_follows = rb_bool(&r);
    if (r.err) return PSSH_E_PROTO;
    if (first_kex_follows) return PSSH_E_KEX; /* a guessed key exchange packet: not sent by OpenSSH; refused */
    const char *name = 0;
    size_t nlen = 0;
    if (!nl_pick(PSSH_KEX_LIST, kex, &name, &nlen)) return PSSH_E_KEX;
    if (!name_is(name, nlen, "curve25519-sha256") && !name_is(name, nlen, "curve25519-sha256@libssh.org"))
        return PSSH_E_KEX;
    const char *strict_name = "kex-strict-s-v00@openssh.com";
    s->strict = nl_has(kex, strict_name, pssh_len(strict_name)) ? 1 : 0;
    if (!nl_pick(PSSH_HOSTKEY_LIST, hk, &name, &nlen)) return PSSH_E_KEX;
    s->hostkey_alg = hostkey_of(name, nlen);
    if (s->hostkey_alg == 0) return PSSH_E_KEX;
    if (!nl_pick(PSSH_CIPHER_LIST, c2s, &name, &nlen)) return PSSH_E_KEX;
    s->cipher_c2s = cipher_of(name, nlen);
    if (!nl_pick(PSSH_CIPHER_LIST, s2c, &name, &nlen)) return PSSH_E_KEX;
    s->cipher_s2c = cipher_of(name, nlen);
    return PSSH_OK;
}

/* ---- host key signature (RFC 8709 ed25519, RFC 4253 section 6.6 rsa, RFC 5656 ecdsa) ---- */
static int verify_host_sig(pssh_t *s, pssh_bytes_t ks, pssh_bytes_t sig, const uint8_t h[32]) {
    const pssh_ops_t *o = &s->ops;
    pssh_rb_t k, g;
    rb_init(&k, ks.p, ks.n);
    rb_init(&g, sig.p, sig.n);
    pssh_bytes_t alg = rb_str(&k);
    pssh_bytes_t salg = rb_str(&g), sb = rb_str(&g);
    if (k.err || g.err) return PSSH_E_PROTO;
    if (bytes_is(alg, "ssh-ed25519")) {
        pssh_bytes_t pk = rb_str(&k);
        if (pk.n != 32 || !bytes_is(salg, "ssh-ed25519") || sb.n != 64) return PSSH_E_PROTO;
        return o->ed25519_verify(o->ctx, pk.p, h, 32, sb.p) == 0 ? PSSH_OK : PSSH_E_KEX;
    }
    if (bytes_is(alg, "ssh-rsa")) {
        pssh_bytes_t e = rb_str(&k), nn = rb_str(&k);
        int halg = bytes_is(salg, "rsa-sha2-512") ? PSSH_H_SHA512 : bytes_is(salg, "rsa-sha2-256") ? PSSH_H_SHA256 : 0;
        if (!halg || k.err) return PSSH_E_PROTO;
        uint8_t dg[64];
        if (o->hash(o->ctx, halg, h, 32, dg) != 0) return PSSH_E_UNSUPPORTED;
        size_t dl = halg == PSSH_H_SHA512 ? 64 : 32;
        while (nn.n && *nn.p == 0) {
            nn.p++;
            nn.n--;
        }
        while (e.n && *e.p == 0) {
            e.p++;
            e.n--;
        }
        int rc = o->rsa_verify(o->ctx, nn.p, nn.n, e.p, e.n, halg, dg, dl, sb.p, sb.n);
        return rc == 0 ? PSSH_OK : rc == PSSH_E_UNSUPPORTED ? PSSH_E_UNSUPPORTED : PSSH_E_KEX;
    }
    if (bytes_is(alg, "ecdsa-sha2-nistp256")) {
        pssh_rb_t kq;
        pssh_bytes_t curve = rb_str(&k), q = rb_str(&k);
        if (k.err || !bytes_is(curve, "nistp256") || q.n != 65 || q.p[0] != 4) return PSSH_E_PROTO;
        if (!bytes_is(salg, "ecdsa-sha2-nistp256")) return PSSH_E_PROTO;
        rb_init(&kq, sb.p, sb.n);
        pssh_bytes_t rr = rb_str(&kq), ss = rb_str(&kq);
        if (kq.err) return PSSH_E_PROTO;
        uint8_t rs[64] = {0};
        if (rr.n > 33 || ss.n > 33) return PSSH_E_PROTO;
        if (rr.n > 0 && rr.p[0] == 0) { rr.p++; rr.n--; }
        if (ss.n > 0 && ss.p[0] == 0) { ss.p++; ss.n--; }
        if (rr.n > 32 || ss.n > 32) return PSSH_E_PROTO;
        pssh_copy(rs + 32 - rr.n, rr.p, rr.n);
        pssh_copy(rs + 64 - ss.n, ss.p, ss.n);
        uint8_t dg[32];
        if (o->hash(o->ctx, PSSH_H_SHA256, h, 32, dg) != 0) return PSSH_E_UNSUPPORTED;
        int rc = o->ecdsa_p256_verify(o->ctx, q.p + 1, dg, rs);
        return rc == 0 ? PSSH_OK : rc == PSSH_E_UNSUPPORTED ? PSSH_E_UNSUPPORTED : PSSH_E_KEX;
    }
    return PSSH_E_PROTO;
}

/* ---- handshake (RFC 4253 section 7) ---- */
static int ident_try(pssh_t *s) {
    for (int lines = 0; lines < 64; lines++) {
        size_t i = 0;
        while (i < s->in_len && s->in[i] != '\n') i++;
        if (i == s->in_len) return s->in_len > 255 ? PSSH_E_PROTO : PSSH_E_AGAIN;
        size_t end = i;
        if (end > 0 && s->in[end - 1] == '\r') end--;
        if (end >= 4 && s->in[0] == 'S' && s->in[1] == 'S' && s->in[2] == 'H' && s->in[3] == '-') {
            int v2 = name_is((const char *)s->in + 4, 4, "2.0-");
            int v199 = name_is((const char *)s->in + 4, 5, "1.99-");
            if (end < 8 || !(v2 || v199)) return PSSH_E_PROTO;
            if (end > sizeof s->v_s - 1) return PSSH_E_PROTO;
            pssh_copy(s->v_s, s->in, end);
            s->v_s[end] = 0;
            s->v_s_len = end;
            in_drop(s, i + 1);
            return PSSH_OK;
        }
        in_drop(s, i + 1); /* banner lines before the identification string are skipped */
    }
    return PSSH_E_PROTO;
}

static int send_kex_init(pssh_t *s) {
    uint8_t q[32];
    if (s->ops.random(s->ops.ctx, s->eph, 32) != 0) return PSSH_E_IO;
    if (s->ops.x25519(s->ops.ctx, q, s->eph, NULL) != 0) return PSSH_E_KEX; /* NULL point: the base point */
    pssh_copy(s->qc, q, 32);
    pssh_wb_t b;
    uint8_t m[64];
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_KEX_ECDH_INIT);
    wb_str(&b, q, 32);
    return pssh_send(s, m, b.n);
}

/* Handles the server's ECDH reply: shared secret, exchange hash, signature and host key check. */
static int handle_kex_reply(pssh_t *s, const uint8_t *p, size_t n) {
    pssh_rb_t r;
    rb_init(&r, p + 1, n - 1);
    pssh_bytes_t ks = rb_str(&r), qs = rb_str(&r), sig = rb_str(&r);
    if (r.err || qs.n != 32) return PSSH_E_PROTO;
    uint8_t k[32], h[32];
    if (s->ops.x25519(s->ops.ctx, k, s->eph, qs.p) != 0) return PSSH_E_KEX;
    pssh_bytes_t parts[7];
    parts[0].p = (const uint8_t *)PSSH_VERSION_LINE; /* V_C: ours */
    parts[0].n = pssh_len(PSSH_VERSION_LINE);
    parts[1].p = (const uint8_t *)s->v_s; /* V_S */
    parts[1].n = s->v_s_len;
    parts[2].p = s->i_c; /* I_C, I_S: the KEXINIT payloads */
    parts[2].n = s->i_c_len;
    parts[3].p = s->i_s;
    parts[3].n = s->i_s_len;
    parts[4] = ks;
    parts[5].p = s->qc;
    parts[5].n = 32;
    parts[6] = qs;
    if (pssh_exchange_hash(&s->ops, parts, k, h) != 0) return PSSH_E_KEX;
    if (verify_host_sig(s, ks, sig, h) != PSSH_OK) return PSSH_E_KEX;
    if (s->hostkey_check) {
        const char *alg = s->hostkey_alg == 1 ? "ssh-ed25519" : s->hostkey_alg == 4 ? "ecdsa-sha2-nistp256" : "ssh-rsa";
        if (s->hostkey_check(s->hostkey_ctx, s->host, s->port, alg, ks.p, ks.n) != 0) return PSSH_E_REFUSED;
    }
    if (!s->have_sid) {
        pssh_copy(s->sid, h, 32);
        s->have_sid = 1;
    }
    pssh_copy(s->kshared, k, 32);
    pssh_copy(s->hsh, h, 32);
    pssh_zero(k, sizeof k);
    pssh_zero(s->eph, sizeof s->eph);
    return PSSH_OK;
}

static int send_newkeys(pssh_t *s) {
    uint8_t m = SSH_MSG_NEWKEYS;
    int rc = pssh_send(s, &m, 1);
    if (rc != PSSH_OK) return rc;
    rc = setup_dir(s, &s->out_dir, s->cipher_c2s, s->kshared, s->hsh, 'A', 'C');
    if (rc == PSSH_OK && s->strict) s->out_dir.seq = 0;
    return rc;
}

static int handle_handshake_msg(pssh_t *s, const uint8_t *p, size_t n, int *state) {
    if (n == 0) return PSSH_E_PROTO;
    switch (p[0]) {
    case SSH_MSG_IGNORE:
    case SSH_MSG_DEBUG:
    case SSH_MSG_EXT_INFO:
    case SSH_MSG_UNIMPLEMENTED:
        return PSSH_OK;
    case SSH_MSG_DISCONNECT:
        return PSSH_E_EOF;
    case SSH_MSG_KEXINIT:
        if (*state != PSSH_HS_KEXINIT) return PSSH_E_PROTO;
        if (kexinit_parse(s, p, n) != PSSH_OK) return PSSH_E_KEX;
        if (send_kex_init(s) != PSSH_OK) return PSSH_E_KEX;
        *state = PSSH_HS_REPLY;
        return PSSH_OK;
    case SSH_MSG_KEX_ECDH_REPLY: {
        if (*state != PSSH_HS_REPLY) return PSSH_E_PROTO;
        int rc = handle_kex_reply(s, p, n);
        if (rc != PSSH_OK) return rc;
        rc = send_newkeys(s);
        if (rc != PSSH_OK) return rc;
        *state = PSSH_HS_NEWKEYS;
        return PSSH_OK;
    }
    case SSH_MSG_NEWKEYS: {
        if (*state != PSSH_HS_NEWKEYS || n != 1) return PSSH_E_PROTO;
        int rc = setup_dir(s, &s->in_dir, s->cipher_s2c, s->kshared, s->hsh, 'B', 'D');
        if (rc != PSSH_OK) return rc;
        if (s->strict) s->in_dir.seq = 0;
        s->kex_done = 1;
        pssh_zero(s->kshared, sizeof s->kshared);
        return PSSH_OK;
    }
    default:
        return PSSH_E_PROTO;
    }
}

/* One round of socket work: flush what is pending, wait for the socket (or an event), read what arrived. */
static int pump_once(pssh_t *s, int64_t deadline) {
    int rc = pssh_flush(s);
    if (rc == PSSH_E_IO) return rc;
    int64_t left = deadline - s->ops.now_ms(s->ops.ctx);
    if (left <= 0) return PSSH_E_TIMEOUT;
    unsigned want = PSSH_W_SOCK_IN | (s->out_len > s->out_off ? PSSH_W_SOCK_OUT : 0u), got = 0;
    rc = wait_io(s, want, (int)left, &got);
    if (rc != PSSH_OK) return rc;
    /* a non-blocking read always runs: data the transport already holds can be there without a readiness bit */
    rc = io_fill(s);
    if (rc == PSSH_E_EOF) return PSSH_E_EOF;
    if (rc < 0 && rc != PSSH_E_AGAIN) return rc;
    return PSSH_OK;
}

/* Writes everything in the output buffer. It waits only for room to write, never for input: the peer may be waiting for
 * what we send next, so waiting for its data here would stall both sides until the timeout. */
static int flush_all(pssh_t *s, int64_t deadline) {
    while (s->out_len > s->out_off) {
        int rc = pssh_flush(s);
        if (rc == PSSH_E_IO) return rc;
        if (rc == PSSH_E_AGAIN) {
            int64_t left = deadline - s->ops.now_ms(s->ops.ctx);
            if (left <= 0) return PSSH_E_TIMEOUT;
            unsigned got = 0;
            rc = wait_io(s, PSSH_W_SOCK_OUT, (int)left, &got);
            if (rc != PSSH_OK) return rc;
        }
    }
    return PSSH_OK;
}

/* Waits for the next complete packet (not the ident line). */
static int next_packet(pssh_t *s, const uint8_t **p, size_t *n, int64_t deadline) {
    for (;;) {
        int rc = pssh_recv_try(s, p, n);
        if (rc != PSSH_E_AGAIN) return rc;
        rc = pump_once(s, deadline);
        if (rc != PSSH_OK) return rc;
    }
}

int pssh_handshake(pssh_t *s, int timeout_ms) {
    if (!s->ops.sock_read || !s->ops.sock_write) return PSSH_E_ARG;
    int64_t deadline = s->ops.now_ms(s->ops.ctx) + timeout_ms;
    int rc;
    s->kex_done = 0;
    /* identification: our line first, then KEXINIT as the first binary packet */
    pssh_copy(s->out, "SSH-2.0-PalanuOS_1.0\r\n", 22);
    s->out_len = 22;
    rc = kexinit_build(s);
    if (rc != PSSH_OK) return rc;
    rc = pssh_send(s, s->i_c, s->i_c_len);
    if (rc != PSSH_OK) return rc;
    int state = PSSH_HS_KEXINIT;
    while (!s->kex_done) {
        const uint8_t *p = 0;
        size_t n = 0;
        if (!s->v_s_len) {
            rc = ident_try(s);
            if (rc == PSSH_OK) continue;
            if (rc != PSSH_E_AGAIN) return rc;
            rc = pump_once(s, deadline);
            if (rc != PSSH_OK) return rc;
            continue;
        }
        rc = next_packet(s, &p, &n, deadline);
        if (rc != PSSH_OK) return rc;
        rc = handle_handshake_msg(s, p, n, &state);
        if (rc != PSSH_OK) return rc;
    }
    /* our NEWKEYS may still wait in the output buffer: it must reach the server before we go on */
    return flush_all(s, deadline);
}
