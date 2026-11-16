/* palssh_hosts.inc: known_hosts in the OpenSSH format (sshd(8) / ssh(1) "FILES"): plain host names with a `[host]:port`
 * form for ports other than 22, comma lists with * and ? wildcards, `!` negation, and hashed entries
 * `|1|base64(salt)|base64(HMAC-SHA1(salt, name))|`. The file is given as text; the app reads and appends it. */

/* Case-insensitive glob: * matches any run, ? one character. */
static int glob_match(const char *pat, size_t pl, const char *str, size_t sl) {
    size_t pi = 0, si = 0, star_p = (size_t)-1, star_s = 0;
    while (si < sl) {
        if (pi < pl && (pat[pi] == '?' || pssh_ceq(pat[pi], str[si]))) {
            pi++;
            si++;
        } else if (pi < pl && pat[pi] == '*') {
            star_p = pi++;
            star_s = si;
        } else if (star_p != (size_t)-1) {
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return 0;
        }
    }
    while (pi < pl && pat[pi] == '*') pi++;
    return pi == pl;
}

/* A hashed host field: |1|salt|hash| (SHA-1 HMAC over the name, keyed with the salt). */
static int hashed_match(const pssh_ops_t *ops, const char *pat, size_t pl, const char *name, size_t nl) {
    size_t i = 3; /* after "|1|" */
    size_t j = i;
    while (j < pl && pat[j] != '|') j++;
    if (j >= pl) return 0;
    uint8_t salt[32], want[32], got[20];
    int sl = pssh_b64_decode(pat + i, j - i, salt, sizeof salt);
    size_t k = j + 1;
    size_t e = k;
    while (e < pl && pat[e] != '|') e++;
    int hl = pssh_b64_decode(pat + k, e - k, want, sizeof want);
    if (sl <= 0 || hl != 20) return 0;
    if (ops->hmac(ops->ctx, PSSH_H_SHA1, salt, (size_t)sl, (const uint8_t *)name, nl, got) != 0) return 0;
    return pssh_eq(got, want, 20);
}

/* Does the comma list of hosts (field 1 of a line) name the host? Negated patterns (!x) veto a match. */
static int hosts_match(const pssh_ops_t *ops, const char *list, size_t ll, const char *name, size_t nl) {
    int matched = 0;
    size_t i = 0;
    while (i < ll) {
        size_t j = i;
        while (j < ll && list[j] != ',') j++;
        size_t pl = j - i;
        const char *pat = list + i;
        if (pl && pat[0] == '|') {
            if (pl > 3 && pat[1] == '1' && pat[2] == '|' && hashed_match(ops, pat, pl, name, nl)) matched = 1;
        } else if (pl && pat[0] == '!') {
            if (glob_match(pat + 1, pl - 1, name, nl)) return 0;
        } else if (pl && glob_match(pat, pl, name, nl)) {
            matched = 1;
        }
        i = j + 1;
    }
    return matched;
}

/* Name as known_hosts writes it: plain for port 22, [host]:port otherwise. */
static size_t hosts_name(const char *host, uint16_t port, char *out, size_t cap) {
    size_t n = 0, hl = pssh_len(host);
    if (port == 22) {
        if (hl + 1 > cap) return 0;
        pssh_copy(out, host, hl);
        n = hl;
    } else {
        char digits[8];
        size_t dn = 0;
        uint32_t v = port;
        do {
            digits[dn++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        if (hl + dn + 4 > cap) return 0;
        out[n++] = '[';
        pssh_copy(out + n, host, hl);
        n += hl;
        out[n++] = ']';
        out[n++] = ':';
        while (dn) out[n++] = digits[--dn];
    }
    out[n] = 0;
    return n;
}

static int hosts_field_is(const char *p, size_t n, const char *s) { return name_is(p, n, s); }

int pssh_hosts_lookup(const pssh_ops_t *ops, const char *text, size_t tlen, const char *host, uint16_t port,
                      const char *alg, const uint8_t *blob, size_t blen) {
    char name[PSSH_HOST_MAX + 16];
    if (hosts_name(host, port, name, sizeof name) == 0) return PSSH_HOST_NEW;
    size_t nl = pssh_len(name), al = pssh_len(alg);
    int changed = 0;
    size_t i = 0;
    while (i < tlen) {
        size_t e = i;
        while (e < tlen && text[e] != '\n') e++;
        size_t ls = i, le = e;
        if (le > ls && text[le - 1] == '\r') le--;
        i = e + 1;
        if (le == ls || text[ls] == '#' || text[ls] == '@') continue;
        /* split: hosts, keytype, key */
        size_t f1 = ls;
        while (f1 < le && text[f1] != ' ' && text[f1] != '\t') f1++;
        size_t f2 = f1;
        while (f2 < le && (text[f2] == ' ' || text[f2] == '\t')) f2++;
        size_t f2e = f2;
        while (f2e < le && text[f2e] != ' ' && text[f2e] != '\t') f2e++;
        size_t f3 = f2e;
        while (f3 < le && (text[f3] == ' ' || text[f3] == '\t')) f3++;
        size_t f3e = f3;
        while (f3e < le && text[f3e] != ' ' && text[f3e] != '\t') f3e++;
        if (f3e == f3) continue;
        if (!hosts_field_is(text + f2, f2e - f2, alg) || f2e - f2 != al) continue;
        if (!hosts_match(ops, text + ls, f1 - ls, name, nl)) continue;
        uint8_t kb[512];
        int kl = pssh_b64_decode(text + f3, f3e - f3, kb, sizeof kb);
        if (kl < 0) continue;
        if ((size_t)kl == blen && pssh_eq(kb, blob, blen)) return PSSH_HOST_FOUND;
        changed = 1;
    }
    return changed ? PSSH_HOST_CHANGED : PSSH_HOST_NEW;
}

int pssh_hosts_line(const char *host, uint16_t port, const char *alg, const uint8_t *blob, size_t blen, char *out,
                    size_t cap) {
    char name[PSSH_HOST_MAX + 16];
    if (hosts_name(host, port, name, sizeof name) == 0) return PSSH_E_ARG;
    size_t nl = pssh_len(name), al = pssh_len(alg);
    char b64[1024];
    size_t bl = pssh_b64_encode(blob, blen, b64, sizeof b64);
    if (bl == 0) return PSSH_E_ARG;
    size_t total = nl + 1 + al + 1 + bl + 1;
    if (total + 1 > cap) return PSSH_E_ARG;
    size_t o = 0;
    pssh_copy(out + o, name, nl);
    o += nl;
    out[o++] = ' ';
    pssh_copy(out + o, alg, al);
    o += al;
    out[o++] = ' ';
    pssh_copy(out + o, b64, bl);
    o += bl;
    out[o++] = '\n';
    out[o] = 0;
    return (int)o;
}
