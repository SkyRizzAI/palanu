/* palssh_util.inc: wire-format builder and reader (RFC 4251 section 5 types), base64, fingerprints and error text.
 * Included by palssh.h under PALSSH_IMPLEMENTATION. No libc: the helpers below are plain loops. */

static void pssh_copy(void *d, const void *s, size_t n) {
    uint8_t *o = (uint8_t *)d;
    const uint8_t *i = (const uint8_t *)s;
    while (n--) *o++ = *i++;
}

static void pssh_zero(void *d, size_t n) {
    volatile uint8_t *o = (volatile uint8_t *)d; /* volatile: secrets are wiped, not optimised away */
    while (n--) *o++ = 0;
}

static int pssh_eq(const void *a, const void *b, size_t n) { /* constant time */
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint8_t d = 0;
    while (n--) d |= (uint8_t)(*x++ ^ *y++);
    return d == 0;
}

static size_t pssh_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int pssh_ceq(char a, char b) { /* ASCII case-insensitive */
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

/* ---- writer: appends wire-format fields to a fixed buffer; err is set (and nothing written) on overflow ---- */
typedef struct {
    uint8_t *p;
    size_t cap;
    size_t n;
    int err;
} pssh_wb_t;

static void wb_init(pssh_wb_t *b, uint8_t *p, size_t cap) {
    b->p = p;
    b->cap = cap;
    b->n = 0;
    b->err = 0;
}

static void wb_raw(pssh_wb_t *b, const void *d, size_t n) {
    if (b->err) return;
    if (n > b->cap - b->n) {
        b->err = 1;
        return;
    }
    pssh_copy(b->p + b->n, d, n);
    b->n += n;
}

static void wb_u8(pssh_wb_t *b, uint8_t v) { wb_raw(b, &v, 1); }

static void wb_u32(pssh_wb_t *b, uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    wb_raw(b, t, 4);
}

static void wb_str(pssh_wb_t *b, const void *d, size_t n) {
    wb_u32(b, (uint32_t)n);
    wb_raw(b, d, n);
}

static void wb_cstr(pssh_wb_t *b, const char *s) { wb_str(b, s, pssh_len(s)); }

/* mpint of an unsigned big-endian integer: leading zeros dropped, 0x00 added when the top bit is set. */
static void wb_mpint(pssh_wb_t *b, const uint8_t *d, size_t n) {
    while (n && *d == 0) {
        d++;
        n--;
    }
    if (n == 0) {
        wb_u32(b, 0);
        return;
    }
    int pad = (d[0] & 0x80) != 0;
    wb_u32(b, (uint32_t)(n + (size_t)pad));
    if (pad) wb_u8(b, 0);
    wb_raw(b, d, n);
}

/* ---- reader: a cursor over one payload; err is set when a field runs past the end ---- */
typedef struct {
    const uint8_t *p;
    size_t n;
    size_t off;
    int err;
} pssh_rb_t;

static void rb_init(pssh_rb_t *r, const uint8_t *p, size_t n) {
    r->p = p;
    r->n = n;
    r->off = 0;
    r->err = 0;
}

static size_t rb_left(const pssh_rb_t *r) { return r->n - r->off; }

static uint8_t rb_u8(pssh_rb_t *r) {
    if (r->err || r->off >= r->n) {
        r->err = 1;
        return 0;
    }
    return r->p[r->off++];
}

static uint32_t rb_u32(pssh_rb_t *r) {
    if (r->err || rb_left(r) < 4) {
        r->err = 1;
        return 0;
    }
    const uint8_t *q = r->p + r->off;
    r->off += 4;
    return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
}

/* A string field: points into the payload (no copy). */
static pssh_bytes_t rb_str(pssh_rb_t *r) {
    pssh_bytes_t out = {0, 0};
    uint32_t n = rb_u32(r);
    if (r->err || n > rb_left(r)) {
        r->err = 1;
        return out;
    }
    out.p = r->p + r->off;
    out.n = n;
    r->off += n;
    return out;
}

static int rb_bool(pssh_rb_t *r) { return rb_u8(r) != 0; }

/* Compares a string field with a C string. */
static int bytes_is(pssh_bytes_t b, const char *s) {
    size_t n = pssh_len(s);
    return b.n == n && pssh_eq(b.p, s, n);
}

/* ---- base64 (RFC 4648, standard alphabet) ---- */
static const char pssh_b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t pssh_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t need = ((n + 2) / 3) * 4;
    if (cap < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = pssh_b64c[(v >> 18) & 63];
        out[o++] = pssh_b64c[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? pssh_b64c[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? pssh_b64c[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int pssh_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int v = b64_val(c);
        if (v < 0) return PSSH_E_PROTO;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return PSSH_E_PROTO;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

/* ---- fingerprints (ssh-keygen -l -E sha256) ---- */
int pssh_fingerprint(const pssh_ops_t *ops, const uint8_t *blob, size_t n, char *out, size_t cap) {
    uint8_t d[32];
    if (ops->hash(ops->ctx, PSSH_H_SHA256, blob, n, d) != 0) return PSSH_E_UNSUPPORTED;
    char b[64];
    size_t bl = pssh_b64_encode(d, sizeof d, b, sizeof b);
    while (bl && b[bl - 1] == '=') bl--; /* ssh-keygen prints the digest without padding */
    if (cap < 8 + bl + 1) return PSSH_E_ARG;
    pssh_copy(out, "SHA256:", 7);
    pssh_copy(out + 7, b, bl);
    out[7 + bl] = 0;
    return (int)(7 + bl);
}

const char *pssh_strerror(int err) {
    switch (err) {
    case PSSH_OK: return "ok";
    case PSSH_E_AGAIN: return "no data yet";
    case PSSH_E_EOF: return "connection closed";
    case PSSH_E_IO: return "network error";
    case PSSH_E_PROTO: return "protocol error";
    case PSSH_E_KEX: return "key exchange failed";
    case PSSH_E_AUTH: return "authentication failed";
    case PSSH_E_REFUSED: return "host key refused";
    case PSSH_E_UNSUPPORTED: return "algorithm not supported by this device";
    case PSSH_E_TIMEOUT: return "timed out";
    case PSSH_E_ARG: return "bad argument";
    case PSSH_E_QUIT: return "interrupted";
    case PSSH_E_CHANNEL: return "session refused";
    case PSSH_E_NOKEY: return "no such key";
    default: return "error";
    }
}

/* Clears the session state and installs the backend. The config fields (user, host, port, key_name, want_pty,
 * hostkey_check) are set by the caller after this. */
int pssh_init(pssh_t *s, const pssh_ops_t *ops) {
    if (!ops || !ops->sock_read || !ops->sock_write || !ops->wait || !ops->random || !ops->hash || !ops->hmac ||
        !ops->x25519 || !ops->ed25519_verify || !ops->chacha20 || !ops->poly1305 || !ops->aead_open ||
        !ops->aead_seal || !ops->aead_decrypt || !ops->now_ms)
        return PSSH_E_ARG;
    pssh_zero(s, sizeof *s);
    s->ops = *ops;
    s->at_bol = 1;
    s->cols = 80;
    s->rows = 24;
    s->port = 22;
    pssh_copy(s->term, "xterm-256color", 15);
    return PSSH_OK;
}
