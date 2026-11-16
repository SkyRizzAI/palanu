/* palssh_pal.inc: the default backend, on the public PalanuOS API (palanu.h). A pssh_pal_t holds the socket handle
 * and the event queue; the app makes one with pssh_pal_init (which sets the socket
 * non-blocking), fills the ops with pssh_pal_ops and calls pssh_init. */

typedef struct {
    int32_t sock;   /* the connected TCP socket (pal_tcp_connect) */
    int32_t events; /* pal_events(): RESIZE and QUIT */
} pssh_pal_t;

/* One record of pal_wait: int32 h, uint16 want, uint16 got (8 bytes). */
typedef struct {
    int32_t h;
    uint16_t want;
    uint16_t got;
} pssh_pal_rec_t;

static int pssh_pal_map(int32_t r) {
    if (r == PAL_ERR_AGAIN) return PSSH_E_AGAIN;
    if (r < 0) return PSSH_E_IO;
    return (int)r;
}

static int pssh_pal_sock_read(void *c, uint8_t *buf, size_t n) {
    pssh_pal_t *p = (pssh_pal_t *)c;
    return pssh_pal_map(pal_read(p->sock, buf, (uint32_t)n));
}

static int pssh_pal_sock_write(void *c, const uint8_t *buf, size_t n) {
    pssh_pal_t *p = (pssh_pal_t *)c;
    return pssh_pal_map(pal_write(p->sock, buf, (uint32_t)n));
}

static int pssh_pal_stdin_read(void *c, uint8_t *buf, size_t n) {
    (void)c;
    return pssh_pal_map(pal_read(0, buf, (uint32_t)n));
}

static int pssh_pal_stdout_write(void *c, int fd, const uint8_t *buf, size_t n) {
    (void)c;
    return pssh_pal_map(pal_write((int32_t)fd, buf, (uint32_t)n));
}

static int pssh_pal_wait(void *c, unsigned want, int timeout_ms, unsigned *got) {
    pssh_pal_t *p = (pssh_pal_t *)c;
    pssh_pal_rec_t r[3];
    int n = 0, is_sock = -1, is_stdin = -1, is_ev = -1;
    *got = 0;
    if (want & (PSSH_W_SOCK_IN | PSSH_W_SOCK_OUT)) {
        r[n].h = p->sock;
        r[n].want = (uint16_t)(((want & PSSH_W_SOCK_IN) ? PAL_POLL_IN : 0) | ((want & PSSH_W_SOCK_OUT) ? PAL_POLL_OUT : 0));
        r[n].got = 0;
        is_sock = n++;
    }
    if (want & PSSH_W_STDIN) {
        r[n].h = 0;
        r[n].want = PAL_POLL_IN;
        r[n].got = 0;
        is_stdin = n++;
    }
    if (want & PSSH_W_EVENT) {
        r[n].h = p->events;
        r[n].want = PAL_POLL_IN;
        r[n].got = 0;
        is_ev = n++;
    }
    int32_t rc = pal_wait(r, (uint32_t)(n * 8), timeout_ms);
    if (rc == PAL_ERR_INTR) { /* a stop request ended the wait */
        *got = PSSH_W_EVENT;
        return 0;
    }
    if (rc < 0) return PSSH_E_IO;
    if (is_sock >= 0 && r[is_sock].got & (PAL_POLL_IN | PAL_POLL_HUP | PAL_POLL_ERR)) *got |= PSSH_W_SOCK_IN;
    if (is_sock >= 0 && r[is_sock].got & PAL_POLL_OUT) *got |= PSSH_W_SOCK_OUT;
    if (is_stdin >= 0 && r[is_stdin].got & PAL_POLL_IN) *got |= PSSH_W_STDIN;
    if (is_ev >= 0 && r[is_ev].got & PAL_POLL_IN) *got |= PSSH_W_EVENT;
    return 0;
}

static int pssh_pal_event(void *c, int *kind, int *cols, int *rows) {
    pssh_pal_t *p = (pssh_pal_t *)c;
    uint8_t ev[16];
    *kind = PSSH_EV_NONE;
    int32_t rc = pal_event_next(ev, sizeof ev, 0);
    if (rc < 0) return PSSH_E_IO;
    if (rc != 16) return 0;
    (void)p;
    uint32_t type = (uint32_t)ev[0] | ((uint32_t)ev[1] << 8) | ((uint32_t)ev[2] << 16) | ((uint32_t)ev[3] << 24);
    int32_t a = (int32_t)((uint32_t)ev[4] | ((uint32_t)ev[5] << 8) | ((uint32_t)ev[6] << 16) | ((uint32_t)ev[7] << 24));
    int32_t b = (int32_t)((uint32_t)ev[8] | ((uint32_t)ev[9] << 8) | ((uint32_t)ev[10] << 16) | ((uint32_t)ev[11] << 24));
    if (type == PAL_EV_QUIT) *kind = PSSH_EV_QUIT;
    if (type == PAL_EV_RESIZE) {
        *kind = PSSH_EV_RESIZE;
        *cols = a;
        *rows = b;
    }
    return 0;
}

static int pssh_pal_size(void *c, int *cols, int *rows) {
    (void)c;
    uint8_t t[4];
    if (pal_tty_size(t, sizeof t) < 0) return PSSH_E_IO;
    *cols = t[0] | (t[1] << 8);
    *rows = t[2] | (t[3] << 8);
    return 0;
}

static int64_t pssh_pal_now(void *c) {
    (void)c;
    return pal_now_ms();
}

static int pssh_pal_random(void *c, uint8_t *buf, size_t n) {
    (void)c;
    return pal_random(buf, (uint32_t)n) == 0 ? 0 : PSSH_E_IO;
}

static int pssh_pal_digest(int32_t h, const uint8_t *d, size_t n, uint8_t *out) {
    if (h < 0) return PSSH_E_UNSUPPORTED;
    if (n && pal_hash_update(h, d, (uint32_t)n) != 0) return PSSH_E_IO;
    return pal_hash_final(h, out, 64) > 0 ? 0 : PSSH_E_IO;
}

static int pssh_pal_hash(void *c, int alg, const uint8_t *d, size_t n, uint8_t *out) {
    (void)c;
    return pssh_pal_digest(pal_hash_open(alg), d, n, out);
}

static int pssh_pal_hmac(void *c, int alg, const uint8_t *key, size_t klen, const uint8_t *d, size_t n, uint8_t *out) {
    (void)c;
    return pssh_pal_digest(pal_hmac_open(alg, key, (uint32_t)klen), d, n, out);
}

static int pssh_pal_x25519(void *c, uint8_t out[32], const uint8_t scalar[32], const uint8_t *point) {
    (void)c;
    if (point) return pal_x25519(out, 32, scalar, 32, point, 32) == 0 ? 0 : PSSH_E_KEX;
    return pal_x25519_base(out, 32, scalar, 32) == 0 ? 0 : PSSH_E_KEX;
}

static int pssh_pal_ed25519(void *c, const uint8_t pk[32], const uint8_t *msg, size_t n, const uint8_t sig[64]) {
    (void)c;
    return pal_ed25519_verify(pk, 32, msg, (uint32_t)n, sig, 64);
}

static int pssh_pal_ecdsa(void *c, const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]) {
    (void)c;
    return pal_ecdsa_p256_verify(pub, 64, hash, 32, sig, 64);
}

static int pssh_pal_rsa(void *c, const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int halg,
                        const uint8_t *hash, size_t hlen, const uint8_t *sig, size_t slen) {
    (void)c;
    return pal_rsa_verify_pkcs1(n, (uint32_t)nlen, e, (uint32_t)elen, halg, hash, (uint32_t)hlen, sig, (uint32_t)slen);
}

static int pssh_pal_chacha(void *c, const uint8_t key[32], const uint8_t nonce[8], uint64_t counter, const uint8_t *in,
                           uint8_t *out, size_t n) {
    (void)c;
    return pal_chacha20_xor(key, 32, nonce, 8, counter, in, (uint32_t)n, out, (uint32_t)n);
}

static int pssh_pal_poly(void *c, const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t tag[16]) {
    (void)c;
    return pal_poly1305(key, 32, msg, (uint32_t)n, tag, 16);
}

static int pssh_pal_aead_open(void *c, int alg, const uint8_t *key, size_t klen) {
    (void)c;
    return pal_aead_open(alg, key, (uint32_t)klen);
}

static int pssh_pal_aead_seal(void *c, int h, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                              size_t n, uint8_t tag[16]) {
    (void)c;
    return pal_aead_seal(h, nonce, 12, aad, (uint32_t)alen, buf, (uint32_t)n, tag, 16);
}

static int pssh_pal_aead_dec(void *c, int h, const uint8_t nonce[12], const uint8_t *aad, size_t alen, uint8_t *buf,
                             size_t n, const uint8_t tag[16]) {
    (void)c;
    return pal_aead_decrypt(h, nonce, 12, aad, (uint32_t)alen, buf, (uint32_t)n, tag, 16);
}

static int pssh_pal_key_load(void *c, const char *name, size_t nlen) {
    (void)c;
    return pal_key_load(name, (uint32_t)nlen);
}

static int pssh_pal_key_public(void *c, int h, uint8_t *out, size_t cap) {
    (void)c;
    return pal_key_public(h, out, (uint32_t)cap);
}

static int pssh_pal_key_sign(void *c, int h, const uint8_t *d, size_t n, uint8_t *sig, size_t cap) {
    (void)c;
    return pal_key_sign(h, d, (uint32_t)n, sig, (uint32_t)cap);
}

/* Reads a line without echo. The terminal mode is put back afterwards. */
static int pssh_pal_secret(void *c, const char *prompt, char *buf, size_t cap) {
    (void)c;
    int saved = pal_tty_get();
    size_t pl = pssh_len(prompt);
    pal_write(1, prompt, (uint32_t)pl);
    pal_tty_mode(PAL_TTY_NOECHO);
    size_t n = 0;
    for (;;) {
        char ch;
        int32_t r = pal_read(0, &ch, 1);
        if (r <= 0 || ch == '\n' || ch == '\r') break;
        if (n + 1 < cap) buf[n++] = ch;
    }
    buf[n] = 0;
    if (saved >= 0) pal_tty_mode(saved == PAL_TTY_NOECHO ? PAL_TTY_COOKED : saved);
    else pal_tty_mode(PAL_TTY_COOKED);
    pal_write(1, "\n", 1);
    return (int)n;
}

/* Makes the backend for a connected socket: sets the socket non-blocking and opens the event queue. */
int pssh_pal_init(pssh_pal_t *p, int32_t sock) {
    p->sock = sock;
    if (pal_set_nonblock(sock, 1) < 0) return PSSH_E_IO;
    p->events = pal_events();
    return p->events < 0 ? PSSH_E_IO : PSSH_OK;
}

/* Fills o with the PAL functions. The app passes o to pssh_init. */
void pssh_pal_ops(pssh_ops_t *o, pssh_pal_t *p) {
    o->ctx = p;
    o->sock_read = pssh_pal_sock_read;
    o->sock_write = pssh_pal_sock_write;
    o->stdin_read = pssh_pal_stdin_read;
    o->stdout_write = pssh_pal_stdout_write;
    o->wait = pssh_pal_wait;
    o->event = pssh_pal_event;
    o->size = pssh_pal_size;
    o->now_ms = pssh_pal_now;
    o->random = pssh_pal_random;
    o->hash = pssh_pal_hash;
    o->hmac = pssh_pal_hmac;
    o->x25519 = pssh_pal_x25519;
    o->ed25519_verify = pssh_pal_ed25519;
    o->ecdsa_p256_verify = pssh_pal_ecdsa;
    o->rsa_verify = pssh_pal_rsa;
    o->chacha20 = pssh_pal_chacha;
    o->poly1305 = pssh_pal_poly;
    o->aead_open = pssh_pal_aead_open;
    o->aead_seal = pssh_pal_aead_seal;
    o->aead_decrypt = pssh_pal_aead_dec;
    o->key_load = pssh_pal_key_load;
    o->key_public = pssh_pal_key_public;
    o->key_sign = pssh_pal_key_sign;
    o->secret = pssh_pal_secret;
}
