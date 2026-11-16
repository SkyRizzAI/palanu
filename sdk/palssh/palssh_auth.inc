/* palssh_auth.inc: service request and user authentication (RFC 4252): none (to learn the methods), publickey with a
 * keystore key (the signature covers the session id, RFC 4252 section 7), keyboard-interactive and password. */

#define PSSH_M_PUBLICKEY 1
#define PSSH_M_KBDINT 2
#define PSSH_M_PASSWORD 4

/* Receives the next packet that is not a notice: ignored messages are skipped, global requests are refused (so the
 * server's keepalives get an answer), and a disconnect comes back as PSSH_E_EOF with its text in banner. */
static int auth_recv(pssh_t *s, int64_t deadline, const uint8_t **p, size_t *n) {
    for (;;) {
        int rc = next_packet(s, p, n, deadline);
        if (rc != PSSH_OK) return rc;
        if (*n == 0) return PSSH_E_PROTO;
        switch ((*p)[0]) {
        case SSH_MSG_IGNORE:
        case SSH_MSG_DEBUG:
        case SSH_MSG_EXT_INFO:
            continue;
        case SSH_MSG_USERAUTH_BANNER: {
            pssh_rb_t r;
            rb_init(&r, *p + 1, *n - 1);
            pssh_bytes_t text = rb_str(&r);
            size_t k = text.n < sizeof s->banner - 1 ? text.n : sizeof s->banner - 1;
            pssh_copy(s->banner, text.p, k);
            s->banner[k] = 0;
            continue;
        }
        case SSH_MSG_GLOBAL_REQUEST: {
            pssh_rb_t r;
            rb_init(&r, *p + 1, *n - 1);
            rb_str(&r);
            int want = rb_bool(&r);
            if (want) {
                uint8_t f = SSH_MSG_REQUEST_FAILURE;
                rc = pssh_send(s, &f, 1);
                if (rc != PSSH_OK) return rc;
            }
            continue;
        }
        case SSH_MSG_DISCONNECT: {
            pssh_rb_t r;
            rb_init(&r, *p + 1, *n - 1);
            rb_u32(&r);
            pssh_bytes_t text = rb_str(&r);
            size_t k = text.n < sizeof s->banner - 1 ? text.n : sizeof s->banner - 1;
            pssh_copy(s->banner, text.p, k);
            s->banner[k] = 0;
            return PSSH_E_EOF;
        }
        default:
            return PSSH_OK;
        }
    }
}

/* Starts a USERAUTH_REQUEST for user with the service "ssh-connection" and the method name. */
static void auth_head(pssh_wb_t *b, pssh_t *s, const char *method) {
    wb_u8(b, SSH_MSG_USERAUTH_REQUEST);
    wb_cstr(b, s->user);
    wb_cstr(b, "ssh-connection");
    wb_cstr(b, method);
}

/* The method list of a USERAUTH_FAILURE, as bits of the PSSH_M_* values we support. */
static unsigned methods_of(pssh_bytes_t list) {
    unsigned m = 0;
    if (nl_has(list, "publickey", 9)) m |= PSSH_M_PUBLICKEY;
    if (nl_has(list, "keyboard-interactive", 20)) m |= PSSH_M_KBDINT;
    if (nl_has(list, "password", 8)) m |= PSSH_M_PASSWORD;
    return m;
}

/* Reads the answer of one attempt: 1 on success, 0 on failure (with the methods the server still allows). */
static int auth_answer(pssh_t *s, int64_t deadline, unsigned *methods) {
    const uint8_t *p = 0;
    size_t n = 0;
    int rc = auth_recv(s, deadline, &p, &n);
    if (rc != PSSH_OK) return rc;
    if (p[0] == SSH_MSG_USERAUTH_SUCCESS) return 1;
    if (p[0] == SSH_MSG_USERAUTH_FAILURE) {
        pssh_rb_t r;
        rb_init(&r, p + 1, n - 1);
        *methods = methods_of(rb_str(&r));
        return 0;
    }
    return PSSH_E_PROTO;
}

/* Public key authentication with the keystore key s->key_name. Returns 1 success, 0 refused, <0 error, or
 * PSSH_E_NOKEY when the key does not exist (the method is then skipped). */
static int auth_publickey(pssh_t *s, int64_t deadline, unsigned *methods) {
    const pssh_ops_t *o = &s->ops;
    int h = o->key_load(o->ctx, s->key_name, pssh_len(s->key_name));
    if (h < 0) return PSSH_E_NOKEY;
    uint8_t pub[256];
    int pl = o->key_public(o->ctx, h, pub, sizeof pub);
    if (pl <= 0) return PSSH_E_NOKEY;
    pssh_rb_t kr;
    rb_init(&kr, pub, (size_t)pl);
    pssh_bytes_t kalg = rb_str(&kr);
    if (kr.err) return PSSH_E_NOKEY;
    int ecdsa = bytes_is(kalg, "ecdsa-sha2-nistp256");
    const char *alg = ecdsa ? "ecdsa-sha2-nistp256" : "ssh-ed25519";
    uint8_t m[512];
    pssh_wb_t b;
    /* query: is this key acceptable? (the server answers PK_OK or FAILURE) */
    wb_init(&b, m, sizeof m);
    auth_head(&b, s, "publickey");
    wb_u8(&b, 0);
    wb_cstr(&b, alg);
    wb_str(&b, pub, (size_t)pl);
    if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    const uint8_t *p = 0;
    size_t n = 0;
    int rc = auth_recv(s, deadline, &p, &n);
    if (rc != PSSH_OK) return rc;
    if (p[0] == SSH_MSG_USERAUTH_FAILURE) {
        pssh_rb_t r;
        rb_init(&r, p + 1, n - 1);
        *methods = methods_of(rb_str(&r));
        return 0;
    }
    if (p[0] != SSH_MSG_USERAUTH_PK_OK) return PSSH_E_PROTO;
    /* signed request: the signature covers session id || the request without the signature field */
    wb_init(&b, m, sizeof m);
    wb_str(&b, s->sid, 32);
    auth_head(&b, s, "publickey");
    wb_u8(&b, 1);
    wb_cstr(&b, alg);
    wb_str(&b, pub, (size_t)pl);
    if (b.err) return PSSH_E_ARG;
    uint8_t raw[128];
    int sl = o->key_sign(o->ctx, h, m, b.n, raw, sizeof raw);
    if (sl <= 0) return PSSH_E_KEX;
    /* the message sent is the same request (without the session id) plus the signature blob */
    uint8_t req[512];
    pssh_wb_t r2;
    wb_init(&r2, req, sizeof req);
    auth_head(&r2, s, "publickey");
    wb_u8(&r2, 1);
    wb_cstr(&r2, alg);
    wb_str(&r2, pub, (size_t)pl);
    /* signature blob: algorithm name, then the signature bytes (ecdsa: mpint r and mpint s inside) */
    uint8_t sigb[160];
    pssh_wb_t sb;
    wb_init(&sb, sigb, sizeof sigb);
    wb_cstr(&sb, alg);
    if (ecdsa) {
        if (sl != 64) return PSSH_E_KEX;
        pssh_wb_t inner;
        uint8_t in[80];
        wb_init(&inner, in, sizeof in);
        wb_mpint(&inner, raw, 32);
        wb_mpint(&inner, raw + 32, 32);
        wb_str(&sb, in, inner.n);
    } else {
        if (sl != 64) return PSSH_E_KEX;
        wb_str(&sb, raw, 64);
    }
    wb_str(&r2, sigb, sb.n);
    if (r2.err || sb.err) return PSSH_E_ARG;
    if (pssh_send(s, req, r2.n) != PSSH_OK) return PSSH_E_IO;
    return auth_answer(s, deadline, methods);
}

/* Keyboard-interactive, answered with the password (one round; more rounds get empty answers). */
static int auth_kbdint(pssh_t *s, int64_t deadline, unsigned *methods) {
    uint8_t m[512];
    pssh_wb_t b;
    wb_init(&b, m, sizeof m);
    auth_head(&b, s, "keyboard-interactive");
    wb_cstr(&b, "");
    wb_cstr(&b, "");
    if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    for (int round = 0; round < 3; round++) {
        const uint8_t *p = 0;
        size_t n = 0;
        int rc = auth_recv(s, deadline, &p, &n);
        if (rc != PSSH_OK) return rc;
        if (p[0] == SSH_MSG_USERAUTH_SUCCESS) return 1;
        if (p[0] == SSH_MSG_USERAUTH_FAILURE) {
            pssh_rb_t r;
            rb_init(&r, p + 1, n - 1);
            *methods = methods_of(rb_str(&r));
            return 0;
        }
        if (p[0] != SSH_MSG_USERAUTH_INFO_REQUEST) return PSSH_E_PROTO;
        pssh_rb_t r;
        rb_init(&r, p + 1, n - 1);
        rb_str(&r);
        rb_str(&r);
        rb_str(&r);
        uint32_t count = rb_u32(&r);
        if (r.err || count > 8) return PSSH_E_PROTO;
        char secret[PSSH_SECRET_MAX];
        int have = 0;
        for (uint32_t i = 0; i < count; i++) {
            rb_str(&r);
            rb_bool(&r);
        }
        if (r.err) return PSSH_E_PROTO;
        if (round == 0 && s->ops.secret) {
            have = s->ops.secret(s->ops.ctx, "Password: ", secret, sizeof secret);
            if (have < 0) have = 0;
        }
        pssh_wb_t rb;
        uint8_t resp[512];
        wb_init(&rb, resp, sizeof resp);
        wb_u8(&rb, SSH_MSG_USERAUTH_INFO_RESPONSE);
        wb_u32(&rb, count);
        for (uint32_t i = 0; i < count; i++) {
            if (round == 0 && i == 0) wb_str(&rb, secret, (size_t)have);
            else wb_str(&rb, "", 0);
        }
        pssh_zero(secret, sizeof secret);
        if (rb.err || pssh_send(s, resp, rb.n) != PSSH_OK) return PSSH_E_IO;
    }
    return 0;
}

/* Password authentication: asks the user without echo. */
static int auth_password(pssh_t *s, int64_t deadline, unsigned *methods) {
    char secret[PSSH_SECRET_MAX];
    if (!s->ops.secret) return PSSH_E_AUTH;
    int have = s->ops.secret(s->ops.ctx, "Password: ", secret, sizeof secret);
    if (have < 0) return have;
    uint8_t m[512];
    pssh_wb_t b;
    wb_init(&b, m, sizeof m);
    auth_head(&b, s, "password");
    wb_u8(&b, 0);
    wb_str(&b, secret, (size_t)have);
    pssh_zero(secret, sizeof secret);
    if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    return auth_answer(s, deadline, methods);
}

int pssh_authenticate(pssh_t *s, int timeout_ms) {
    int64_t deadline = s->ops.now_ms(s->ops.ctx) + timeout_ms;
    uint8_t m[128];
    pssh_wb_t b;
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_SERVICE_REQUEST);
    wb_cstr(&b, "ssh-userauth");
    if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    const uint8_t *p = 0;
    size_t n = 0;
    int rc = auth_recv(s, deadline, &p, &n);
    if (rc != PSSH_OK) return rc;
    if (p[0] != SSH_MSG_SERVICE_ACCEPT) return PSSH_E_PROTO;
    /* "none" tells us which methods the server offers */
    wb_init(&b, m, sizeof m);
    auth_head(&b, s, "none");
    if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    unsigned methods = 0, tried = 0;
    rc = auth_answer(s, deadline, &methods);
    if (rc == 1) return PSSH_OK;
    if (rc < 0) return rc;
    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned pick = 0;
        if (!(tried & PSSH_M_PUBLICKEY) && (methods & PSSH_M_PUBLICKEY) && s->key_name[0]) pick = PSSH_M_PUBLICKEY;
        else if (!(tried & PSSH_M_KBDINT) && (methods & PSSH_M_KBDINT)) pick = PSSH_M_KBDINT;
        else if (!(tried & PSSH_M_PASSWORD) && (methods & PSSH_M_PASSWORD)) pick = PSSH_M_PASSWORD;
        if (!pick) break;
        tried |= pick;
        int ok;
        if (pick == PSSH_M_PUBLICKEY) {
            ok = auth_publickey(s, deadline, &methods);
            if (ok == PSSH_E_NOKEY) continue; /* no such key: not an error, the next method is tried */
        } else if (pick == PSSH_M_KBDINT) {
            ok = auth_kbdint(s, deadline, &methods);
        } else {
            ok = auth_password(s, deadline, &methods);
        }
        if (ok == 1) return PSSH_OK;
        if (ok < 0) return ok;
    }
    return PSSH_E_AUTH;
}
