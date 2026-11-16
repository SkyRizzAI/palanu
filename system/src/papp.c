/* The .papp container, both formats (docs/PAPP.md). Parsing is pure apart from the process-owned memory a compressed
 * section needs: it checks the hash before it believes a byte, reads the manifest, and hands back the module. Who may
 * install or run a package is decided elsewhere (papp_pkg.c, trust.c). */
#include "pal/papp.h"

#include <stdio.h>
#include <string.h>

#include "../../third_party/miniz/miniz.h" /* tinfl (third_party/miniz) */
#include "pal/sha256.h"

#define V1_HEADER_LEN 20u
#define V1_FLAG_COMPRESSED 0x1u
#define V2_FIXED_LEN 10u   /* magic, fmt, flags, hdr_len, nsect */
#define V2_SECT_LEN 14u    /* type, flags, off, stored, raw */
#define V2_SECT_MAX 8u
#define V2_FLAG_SIGNED 0x2u
#define V2_SECT_DEFLATE 0x1u
#define SIG_TRAILER_LEN 68u /* "SIG0" and the 64-byte Ed25519 signature over the hash */
#define MANIFEST_MAX 4096u
#define ICON_MAX (16u * 1024u)
#define RES_MAX (256u * 1024u)

enum { SEC_MANIFEST = 1, SEC_CODE = 2, SEC_ICON = 3, SEC_RES = 4, SEC_AOT = 5 };

/* ---- a small CBOR reader: just enough for the manifest (maps, arrays, text, unsigned, bool) ---- */

typedef struct {
    const uint8_t *p, *end;
} cur_t;

static bool head(cur_t *c, int *major, uint64_t *arg) {
    if (c->p >= c->end) return false;
    uint8_t b = *c->p++;
    *major = b >> 5;
    int info = b & 31;
    if (info < 24) {
        *arg = (uint64_t)info;
        return true;
    }
    int bytes = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
    if (bytes == 0 || c->end - c->p < bytes) return false; /* indefinite lengths are not accepted */
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v = (v << 8) | *c->p++;
    *arg = v;
    return true;
}

static bool skip(cur_t *c, int depth) {
    int m;
    uint64_t n;
    if (depth > 8 || !head(c, &m, &n)) return false;
    switch (m) {
    case 0: case 1: case 7: return true;
    case 2: case 3:
        if ((uint64_t)(c->end - c->p) < n) return false;
        c->p += n;
        return true;
    case 4:
        for (uint64_t i = 0; i < n; i++)
            if (!skip(c, depth + 1)) return false;
        return true;
    case 5:
        for (uint64_t i = 0; i < n * 2; i++)
            if (!skip(c, depth + 1)) return false;
        return true;
    default: return false;
    }
}

static bool text(cur_t *c, const char **s, size_t *len) {
    int m;
    uint64_t n;
    if (!head(c, &m, &n) || m != 3 || (uint64_t)(c->end - c->p) < n) return false;
    *s = (const char *)c->p;
    *len = (size_t)n;
    c->p += n;
    return true;
}

static bool uint_val(cur_t *c, uint32_t *v) {
    int m;
    uint64_t n;
    if (!head(c, &m, &n) || m != 0 || n > 0xFFFFFFFFu) return false;
    *v = (uint32_t)n;
    return true;
}

static bool bool_val(cur_t *c, bool *v) {
    int m;
    uint64_t n;
    if (!head(c, &m, &n) || m != 7 || (n != 20 && n != 21)) return false;
    *v = n == 21;
    return true;
}

static bool copy_text(cur_t *c, char *dst, size_t cap) {
    const char *s;
    size_t n;
    if (!text(c, &s, &n) || n >= cap) return false;
    memcpy(dst, s, n);
    dst[n] = 0;
    return true;
}

static bool key_is(const char *k, size_t kn, const char *name) { return strlen(name) == kn && memcmp(k, name, kn) == 0; }

static bool id_ok(const char *id) {
    if (!*id) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

/* A permission name maps to its bit; unknown names are refused by the caller, never ignored. */
static bool perm_bit(const char *s, size_t sn, uint32_t *bit) {
    static const struct {
        const char *name;
        uint32_t bit;
    } T[] = {
        {"fs", PAL_PERM_FS},
        {"fs.home", PAL_PERM_FS | PAL_PERM_FS_HOME},
        {"fs.media", PAL_PERM_FS | PAL_PERM_FS_MEDIA},
        {"net", PAL_PERM_NET},
        {"net.listen", PAL_PERM_NET_LISTEN},
        {"proc.spawn", PAL_PERM_PROC},
        {"proc", PAL_PERM_PROC},
        {"crypto.keys", PAL_PERM_CRYPTO_KEYS},
        {"settings.read", PAL_PERM_SETTINGS_READ},
        {"settings.write", PAL_PERM_SETTINGS_WRITE},
        {"notify", PAL_PERM_NOTIFY},
        {"svc.control", PAL_PERM_SVC_CONTROL},
        {"link", PAL_PERM_LINK},
        {"dev", PAL_PERM_DEV},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (key_is(s, sn, T[i].name)) {
            *bit = T[i].bit;
            return true;
        }
    return false;
}

/* An unknown permission is refused (UNSUPPORTED), never ignored; a malformed list is INVAL. */
static nema_err_t perm_list(cur_t *c, uint32_t *perms) {
    int major;
    uint64_t n;
    if (!head(c, &major, &n) || major != 4 || n > 16) return NEMA_ERR_INVAL;
    for (uint64_t j = 0; j < n; j++) {
        const char *s;
        size_t sn;
        uint32_t bit;
        if (!text(c, &s, &sn)) return NEMA_ERR_INVAL;
        if (!perm_bit(s, sn, &bit)) return NEMA_ERR_UNSUPPORTED;
        *perms |= bit;
    }
    return NEMA_OK;
}

static nema_err_t parse_kind(const char *k, pal_papp_t *out) {
    /* format 1 said "fg" (a command run from the shell) and "bg"; both map to the new kinds */
    if (strcmp(k, "service") == 0 || strcmp(k, "bg") == 0) out->kind = PAL_KIND_SERVICE;
    else if (strcmp(k, "tty") == 0 || strcmp(k, "fg") == 0) out->kind = PAL_KIND_TTY;
    else if (strcmp(k, "gui") == 0) out->kind = PAL_KIND_GUI;
    else return NEMA_ERR_UNSUPPORTED;
    return NEMA_OK;
}

static nema_err_t parse_manifest(const uint8_t *m, size_t len, pal_papp_t *out) {
    cur_t c = {m, m + len};
    int major;
    uint64_t pairs;
    if (!head(&c, &major, &pairs) || major != 5 || pairs > 48) return NEMA_ERR_INVAL;
    bool have_kind = false;
    for (uint64_t i = 0; i < pairs; i++) {
        const char *k;
        size_t kn;
        if (!text(&c, &k, &kn)) return NEMA_ERR_INVAL;
        bool ok = true;
        if (key_is(k, kn, "id")) ok = copy_text(&c, out->id, sizeof(out->id));
        else if (key_is(k, kn, "name")) ok = copy_text(&c, out->name, sizeof(out->name));
        else if (key_is(k, kn, "ver")) ok = copy_text(&c, out->version, sizeof(out->version));
        else if (key_is(k, kn, "icon")) ok = copy_text(&c, out->icon, sizeof(out->icon));
        else if (key_is(k, kn, "category")) ok = copy_text(&c, out->category, sizeof(out->category));
        else if (key_is(k, kn, "hidden")) ok = bool_val(&c, &out->hidden);
        else if (key_is(k, kn, "api")) {
            uint32_t v = 0;
            ok = uint_val(&c, &v) && v <= 0xFFFF;
            out->api = (uint16_t)v;
        } else if (key_is(k, kn, "mem")) ok = uint_val(&c, &out->mem_kb);
        else if (key_is(k, kn, "stack")) ok = uint_val(&c, &out->stack_kb);
        else if (key_is(k, kn, "fuel")) ok = uint_val(&c, &out->fuel_m);
        else if (key_is(k, kn, "kind")) {
            char kind[12];
            ok = copy_text(&c, kind, sizeof(kind)) && parse_kind(kind, out) == NEMA_OK;
            have_kind = ok;
        } else if (key_is(k, kn, "perm") || key_is(k, kn, "caps")) {
            nema_err_t pr = perm_list(&c, &out->perms);
            if (pr != NEMA_OK) return pr;
            ok = true;
        } else ok = skip(&c, 0); /* unknown keys are allowed so tools can add their own */
        if (!ok) return NEMA_ERR_INVAL;
    }
    if (!have_kind) out->kind = PAL_KIND_TTY; /* format 1 without a kind: a command, as it always was */
    out->background = out->kind == PAL_KIND_SERVICE;
    if (!id_ok(out->id) || out->api == 0) return NEMA_ERR_INVAL;
    /* API level 1 manifests predate the fs namespace: their "fs" meant /home, so it still does */
    if (out->api < 2 && (out->perms & PAL_PERM_FS)) out->perms |= PAL_PERM_FS_HOME;
    return NEMA_OK;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Process-owned memory for a package's bytes: the calling process when there is one, otherwise the papp subsystem. */
static nema_owner_t loader_owner(void) {
    nema_proc_t *p = nema_self();
    return p ? p->owner : nema_owner_register("papp");
}

static uint8_t *alloc_bytes(size_t n) {
    uint8_t *b = nema_alloc(NEMA_MEM_EXTERNAL, loader_owner(), n ? n : 1);
    if (!b) b = nema_alloc(NEMA_MEM_ANY, loader_owner(), n ? n : 1);
    return b;
}

/* Raw DEFLATE (RFC 1951) of exactly `raw` bytes, inflated into process-owned memory. The inflater state is large, so it
 * lives on the heap too: an app's task stack is small. */
static nema_err_t inflate(const uint8_t *src, size_t stored, size_t raw, uint8_t **dst) {
    uint8_t *d = alloc_bytes(raw);
    if (!d) return NEMA_ERR_NOMEM;
    tinfl_decompressor *t = (tinfl_decompressor *)nema_alloc(NEMA_MEM_ANY, loader_owner(), sizeof(*t));
    if (!t) {
        nema_free(d);
        return NEMA_ERR_NOMEM;
    }
    tinfl_init(t);
    size_t in = stored, out = raw;
    tinfl_status st = tinfl_decompress(t, src, &in, d, d, &out, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    nema_free(t);
    if (st != TINFL_STATUS_DONE || out != raw) {
        nema_free(d);
        return NEMA_ERR_PROTO; /* a corrupt stream, or one that does not produce the size the header states */
    }
    *dst = d;
    return NEMA_OK;
}

/* Format 1: one header, a manifest, the module, and the hash. */
static nema_err_t parse_v1(const uint8_t *f, size_t len, pal_papp_t *out) {
    if (len < V1_HEADER_LEN + 32) return NEMA_ERR_PROTO;
    uint8_t flags = f[5];
    uint16_t hlen = le16(f + 6);
    uint32_t mlen = le32(f + 8), plen = le32(f + 12), orig = le32(f + 16);
    if (hlen < V1_HEADER_LEN) return NEMA_ERR_UNSUPPORTED;
    if (flags & ~V1_FLAG_COMPRESSED) return NEMA_ERR_UNSUPPORTED; /* a v1 signature trailer does not exist */
    if (flags & V1_FLAG_COMPRESSED) return NEMA_ERR_UNSUPPORTED;  /* format 1 never compressed */
    if (plen != orig || plen > PAL_PAPP_MAX_BYTES) return NEMA_ERR_RANGE;
    size_t need = (size_t)hlen + mlen + plen + 32;
    if (mlen > MANIFEST_MAX || need != len) return NEMA_ERR_PROTO;

    pal_sha256_t h;
    pal_sha256_init(&h);
    pal_sha256_update(&h, f, need - 32);
    pal_sha256_final(&h, out->digest);
    if (memcmp(out->digest, f + need - 32, 32) != 0) return NEMA_ERR_PERM; /* nothing below is trusted before this */

    nema_err_t r = parse_manifest(f + hlen, mlen, out);
    if (r != NEMA_OK) return r;
    if (out->api > PAL_PAPP_API) return NEMA_ERR_UNSUPPORTED;
    out->format = PAL_PAPP_FORMAT_V1;
    out->payload = f + hlen + mlen;
    out->payload_len = plen;
    return NEMA_OK;
}

typedef struct {
    uint8_t type, flags;
    uint32_t off, stored, raw;
} sect_t;

static uint32_t sect_cap(uint8_t type) {
    switch (type) {
    case SEC_MANIFEST: return MANIFEST_MAX;
    case SEC_CODE: return PAL_PAPP_MAX_BYTES;
    case SEC_ICON: return ICON_MAX;
    case SEC_RES: return RES_MAX;
    default: return 0;
    }
}

/* Format 2: header, section table, sections, SHA-256, optional "SIG0" + signature. Every section is checked against the
 * file before anything is read from it, and the hash is checked before the manifest is parsed. */
static nema_err_t parse_v2(const uint8_t *f, size_t len, pal_papp_t *out) {
    if (len < V2_FIXED_LEN + 32) return NEMA_ERR_PROTO;
    uint8_t flags = f[5];
    if (flags & ~V2_FLAG_SIGNED) return NEMA_ERR_UNSUPPORTED;
    uint16_t hlen = le16(f + 6);
    uint16_t nsect = le16(f + 8);
    if (nsect < 2 || nsect > V2_SECT_MAX || hlen != V2_FIXED_LEN + nsect * V2_SECT_LEN) return NEMA_ERR_PROTO;
    bool signed_pkg = (flags & V2_FLAG_SIGNED) != 0;
    size_t tail = 32 + (signed_pkg ? SIG_TRAILER_LEN : 0);
    if (len < (size_t)hlen + tail || len > PAL_PAPP_MAX_FILE + SIG_TRAILER_LEN + 32) return NEMA_ERR_RANGE;
    size_t hash_at = len - tail;
    if (signed_pkg && memcmp(f + hash_at + 32, "SIG0", 4) != 0) return NEMA_ERR_PROTO;

    sect_t s[V2_SECT_MAX];
    bool seen[6] = {false, false, false, false, false, false};
    for (uint16_t i = 0; i < nsect; i++) {
        const uint8_t *e = f + V2_FIXED_LEN + i * V2_SECT_LEN;
        s[i].type = e[0];
        s[i].flags = e[1];
        s[i].off = le32(e + 2);
        s[i].stored = le32(e + 6);
        s[i].raw = le32(e + 10);
        if (s[i].type == SEC_AOT) return NEMA_ERR_UNSUPPORTED; /* reserved for precompiled code: not accepted yet */
        if (s[i].type < SEC_MANIFEST || s[i].type > SEC_RES) return NEMA_ERR_UNSUPPORTED; /* unknown section type */
        if (seen[s[i].type]) return NEMA_ERR_PROTO;
        seen[s[i].type] = true;
        if (s[i].flags & ~V2_SECT_DEFLATE) return NEMA_ERR_UNSUPPORTED;
        if (s[i].off < hlen || s[i].off > hash_at || s[i].stored > hash_at - s[i].off) return NEMA_ERR_PROTO;
        if (!(s[i].flags & V2_SECT_DEFLATE) && s[i].stored != s[i].raw) return NEMA_ERR_PROTO;
        if (s[i].raw > sect_cap(s[i].type)) return NEMA_ERR_RANGE;
    }
    if (!seen[SEC_MANIFEST] || !seen[SEC_CODE]) return NEMA_ERR_PROTO;

    pal_sha256_t h;
    pal_sha256_init(&h);
    pal_sha256_update(&h, f, hash_at);
    pal_sha256_final(&h, out->digest);
    if (memcmp(out->digest, f + hash_at, 32) != 0) return NEMA_ERR_PERM; /* nothing below is trusted before this */

    nema_err_t r = NEMA_OK;
    for (uint16_t i = 0; i < nsect && r == NEMA_OK; i++) {
        if (s[i].type != SEC_MANIFEST) continue;
        const uint8_t *m = f + s[i].off;
        uint8_t *tmp = NULL;
        if (s[i].flags & V2_SECT_DEFLATE) {
            r = inflate(m, s[i].stored, s[i].raw, &tmp);
            m = tmp;
        }
        if (r == NEMA_OK) r = parse_manifest(m, s[i].raw, out);
        if (tmp) nema_free(tmp);
    }
    if (r != NEMA_OK) return r;
    if (out->api > PAL_PAPP_API) return NEMA_ERR_UNSUPPORTED;

    for (uint16_t i = 0; i < nsect && r == NEMA_OK; i++) {
        if (s[i].type != SEC_CODE) continue;
        if (s[i].flags & V2_SECT_DEFLATE) {
            r = inflate(f + s[i].off, s[i].stored, s[i].raw, &out->owned);
            if (r == NEMA_OK) out->payload = out->owned;
        } else {
            out->payload = f + s[i].off;
        }
        out->payload_len = s[i].raw;
    }
    if (r != NEMA_OK) return r;
    out->format = PAL_PAPP_FORMAT;
    out->sig = NULL;
    if (signed_pkg) {
        memcpy(out->sigbuf, f + len - 64, 64);
        out->sig = out->sigbuf;
    }
    return NEMA_OK;
}

nema_err_t pal_papp_parse(const uint8_t *f, size_t len, pal_papp_t *out) {
    memset(out, 0, sizeof(*out));
    if (len < 5 || memcmp(f, PAL_PAPP_MAGIC, 4) != 0) return NEMA_ERR_PROTO;
    nema_err_t r;
    if (f[4] == PAL_PAPP_FORMAT_V1) r = parse_v1(f, len, out);
    else if (f[4] == PAL_PAPP_FORMAT) r = parse_v2(f, len, out);
    else return NEMA_ERR_UNSUPPORTED;
    if (r != NEMA_OK) pal_papp_release(out);
    return r;
}

void pal_papp_release(pal_papp_t *app) {
    if (!app) return;
    if (app->owned) nema_free(app->owned);
    app->owned = NULL;
}

const char *pal_papp_strerror(nema_err_t e) {
    switch (e) {
    case NEMA_ERR_PROTO: return "not a valid package (bad magic, size, or section table)";
    case NEMA_ERR_UNSUPPORTED: return "needs a newer system, or uses an option this system does not have";
    case NEMA_ERR_RANGE: return "package too large";
    case NEMA_ERR_PERM: return "hash mismatch: the package is damaged or was altered";
    case NEMA_ERR_INVAL: return "bad manifest";
    default: return nema_err_str(e);
    }
}

/* ---- files ---- */

nema_err_t pal_papp_load(const char *path, uint8_t **buf, pal_papp_t *out) {
    nema_stat_t st;
    nema_err_t r = nema_fs_stat(path, &st);
    if (r != NEMA_OK) return r;
    if (st.type != NEMA_T_FILE) return NEMA_ERR_ISDIR;
    if (st.size > PAL_PAPP_MAX_FILE) return NEMA_ERR_RANGE;
    nema_obj_t *f;
    r = nema_fs_open(path, NEMA_O_READ, &f);
    if (r != NEMA_OK) return r;
    uint8_t *b = alloc_bytes(st.size);
    if (!b) {
        nema_obj_unref(f);
        return NEMA_ERR_NOMEM;
    }
    size_t got = 0;
    while (got < st.size) {
        int n = nema_obj_read(f, b + got, st.size - got, 2000);
        if (n <= 0) break;
        got += (size_t)n;
    }
    nema_obj_unref(f);
    if (got != st.size) {
        nema_free(b);
        return NEMA_ERR_IO;
    }
    r = pal_papp_parse(b, got, out);
    if (r != NEMA_OK) {
        nema_free(b);
        return r;
    }
    if (out->owned) { /* the inflated module is the buffer now: the file bytes are no longer needed */
        nema_free(b);
        *buf = out->owned;
        out->owned = NULL;
    } else {
        *buf = b;
    }
    return NEMA_OK;
}
