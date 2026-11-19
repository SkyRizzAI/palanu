/* SSH profiles (ssh_config.h). Keywords are case-insensitive; a value may be quoted; comments start with '#'. */
#include "ssh_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libapp.h"

static bool eq_ci(const char *a, size_t n, const char *b) {
    if (strlen(b) != n) return false;
    for (size_t i = 0; i < n; i++)
        if ((a[i] | 32) != (b[i] | 32)) return false;
    return true;
}

/* A glob with * and ? against the whole name. */
static bool glob(const char *p, size_t pn, const char *s) {
    if (pn == 0) return *s == 0;
    if (*p == '*') {
        for (const char *t = s;; t++) {
            if (glob(p + 1, pn - 1, t)) return true;
            if (!*t) return false;
        }
    }
    if (!*s) return false;
    return (*p == '?' || *p == *s) && glob(p + 1, pn - 1, s + 1);
}

/* Does the Host line's list (words, a leading ! negates) take alias? */
static bool host_matches(const char *v, size_t n, const char *alias) {
    bool hit = false;
    for (size_t i = 0; i < n;) {
        while (i < n && (v[i] == ' ' || v[i] == '\t')) i++;
        size_t s = i;
        while (i < n && v[i] != ' ' && v[i] != '\t') i++;
        if (i == s) break;
        bool neg = v[s] == '!';
        if (glob(v + s + neg, i - s - neg, alias)) {
            if (neg) return false;
            hit = true;
        }
    }
    return hit;
}

static void take(char *dst, size_t cap, const char *v, size_t n) {
    if (dst[0]) return; /* the first value wins */
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') v++, n -= 2;
    if (n >= cap) n = cap - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

bool ssh_config_lookup(const char *text, size_t len, const char *alias, ssh_profile_t *out) {
    memset(out, 0, sizeof(*out));
    bool in = true, any = false; /* lines before the first Host apply to every host */
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *a = p;
        p = nl ? nl + 1 : end;
        while (a < le && (*a == ' ' || *a == '\t')) a++;
        if (a >= le) continue;
        const char *hash = memchr(a, '#', (size_t)(le - a)); /* le > a here */
        if (hash) le = hash;
        while (le > a && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
        if (a >= le) continue;
        const char *k = a;
        while (a < le && *a != ' ' && *a != '\t' && *a != '=') a++;
        size_t kn = (size_t)(a - k);
        while (a < le && (*a == ' ' || *a == '\t' || *a == '=')) a++;
        size_t vn = (size_t)(le - a);
        if (eq_ci(k, kn, "host")) {
            in = host_matches(a, vn, alias);
            any |= in;
        } else if (in) {
            if (eq_ci(k, kn, "hostname")) take(out->hostname, sizeof(out->hostname), a, vn);
            else if (eq_ci(k, kn, "user")) take(out->user, sizeof(out->user), a, vn);
            else if (eq_ci(k, kn, "identityfile")) take(out->identity, sizeof(out->identity), a, vn);
            else if (eq_ci(k, kn, "port") && !out->port) out->port = atoi(a);
        }
    }
    if (!strncmp(out->identity, "~/", 2)) { /* ~ is /home on the device */
        char t[96];
        snprintf(t, sizeof(t), "/home/%s", out->identity + 2);
        nema_strlcpy(out->identity, t, sizeof(out->identity));
    }
    return any;
}

bool ssh_config_load(const char *alias, ssh_profile_t *out) {
    nema_obj_t *f;
    memset(out, 0, sizeof(*out));
    if (nema_fs_open("/home/.ssh/config", NEMA_O_READ, &f) != NEMA_OK) return false;
    char *buf = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, 4096);
    int n = buf ? nema_obj_read(f, buf, 4095, -1) : 0;
    nema_obj_unref(f);
    bool ok = n > 0 && ssh_config_lookup(buf, (size_t)n, alias, out);
    nema_free(buf);
    return ok;
}
