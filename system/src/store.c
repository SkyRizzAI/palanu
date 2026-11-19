/* App catalogue (pal/store.h). The list is small (a few KB): it is read whole into memory and parsed into entries. A package is
 * streamed to /tmp while its SHA-256 is computed, then installed from there and removed. */
#include "pal/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cryptoimpl.h"
#include "nema/nema.h"
#include "nema/vfs.h"
#include "pal/board.h"
#include "pal/http.h"
#include "pal/ota.h"
#include "pal/papp.h"
#include "pal/registry.h"
#include "pal/trust.h"

#define LIST_MAX (24u * 1024u)
#define DEFAULT_URL "https://raw.githubusercontent.com/SkyRizzAI/palanu/main/store/index.txt"

static pal_store_entry_t s_e[PAL_STORE_MAX] NEMA_EXT_BSS;
static size_t s_n;
static pal_store_entry_t s_fw[4] NEMA_EXT_BSS; /* the firmware lines ("firmware:<board>"), kept apart from the apps */
static size_t s_nfw;
static bool s_signed; /* index.txt.sig verified with a trusted key */
static char s_base[PAL_HTTP_URL_MAX] NEMA_EXT_BSS; /* the list's URL: package paths are relative to it */

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

int pal_store_parse(const char *text, size_t len, pal_store_entry_t *out, size_t max) {
    const char *p = text, *end = text + len;
    const char *nl = memchr(p, '\n', len);
    size_t first = nl ? (size_t)(nl - p) : len;
    if (first < 14 || strncmp(p, "palanu-store 1", 14)) return -1;
    p = nl ? nl + 1 : end;
    size_t n = 0;
    while (p < end && n < max) {
        nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *f[7];
        size_t fl[7];
        int k = 0;
        for (const char *q = p; k < 7;) { /* fields up to a tab; the last one takes the rest of the line */
            const char *t = k < 6 ? memchr(q, '\t', (size_t)(le - q)) : NULL;
            f[k] = q;
            fl[k] = (size_t)((t ? t : le) - q);
            k++;
            if (!t) break;
            q = t + 1;
        }
        p = nl ? nl + 1 : end;
        if (k < 6 || fl[0] == 0 || fl[4] != 64) continue; /* a bad line is skipped */
        pal_store_entry_t e;
        memset(&e, 0, sizeof(e));
#define CP(dst, i) snprintf(dst, sizeof(dst), "%.*s", (int)(fl[i] < sizeof(dst) ? fl[i] : sizeof(dst) - 1), f[i])
        CP(e.id, 0);
        CP(e.version, 1);
        CP(e.name, 2);
        CP(e.path, 5);
        if (k > 6) CP(e.summary, 6);
#undef CP
        e.size = (uint32_t)strtoul(f[3], NULL, 10);
        bool ok = true;
        for (int i = 0; i < 32 && ok; i++) {
            int hi = hexv(f[4][2 * i]), lo = hexv(f[4][2 * i + 1]);
            ok = hi >= 0 && lo >= 0;
            e.sha256[i] = (uint8_t)(hi << 4 | lo);
        }
        if (ok) out[n++] = e;
    }
    return (int)n;
}

typedef struct {
    char *buf;
    size_t len, cap;
} mem_sink_t;

static int to_mem(void *ctx, const void *b, size_t n) {
    mem_sink_t *m = (mem_sink_t *)ctx;
    if (m->len + n > m->cap) return -1; /* too big for a list */
    memcpy(m->buf + m->len, b, n);
    m->len += n;
    return 0;
}

/* The list's URL: the first line of /etc/store.url, or the project's catalogue. */
static void list_url(void) {
    s_base[0] = 0;
    nema_obj_t *f;
    if (nema_fs_open("/etc/store.url", NEMA_O_READ, &f) == NEMA_OK) {
        int n = nema_obj_read(f, s_base, sizeof(s_base) - 1, -1);
        nema_obj_unref(f);
        s_base[n > 0 ? n : 0] = 0;
        s_base[strcspn(s_base, "\r\n \t")] = 0;
    }
    if (strncmp(s_base, "http://", 7) && strncmp(s_base, "https://", 8)) nema_strlcpy(s_base, DEFAULT_URL, sizeof(s_base));
}

/* index.txt.sig next to the list: 64 bytes, Ed25519 over the list's SHA-256, by a key the device trusts. */
static bool list_signed(const uint8_t sum[32]) {
    /* on the heap: the caller's frame already holds the list's request (ramcheck's stack limit) */
    struct {
        char url[PAL_HTTP_URL_MAX];
        char sig[96];
        uint8_t keys[8][32];
    } *w = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*w));
    if (!w) return false;
    bool ok = false;
    if (snprintf(w->url, sizeof(w->url), "%s.sig", s_base) < (int)sizeof(w->url)) {
        mem_sink_t m = {w->sig, 0, sizeof(w->sig)};
        pal_http_req_t q = {.url = w->url, .redirects = 5, .timeout_ms = 10000, .on_data = to_mem, .ctx = &m};
        pal_http_resp_t r;
        if (pal_http_request(&q, &r) == NEMA_OK && r.status == 200 && m.len == 64) {
            size_t nk = psys_trust_load(w->keys, 8);
            ok = psys_trust_verify(sum, (const uint8_t *)w->sig, (const uint8_t(*)[32])w->keys, nk) == PAL_SIG_TRUSTED;
        }
    }
    nema_free(w);
    return ok;
}

nema_err_t pal_store_refresh(char *msg, size_t msgcap) {
    if (msg && msgcap) msg[0] = 0;
    list_url();
    mem_sink_t m = {nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, LIST_MAX), 0, LIST_MAX};
    if (!m.buf) return NEMA_ERR_NOMEM;
    pal_http_req_t q = {.url = s_base, .redirects = 5, .timeout_ms = 15000, .on_data = to_mem, .ctx = &m};
    pal_http_resp_t r;
    nema_err_t e = pal_http_request(&q, &r);
    if (e == NEMA_OK && r.status != 200) {
        if (msg) snprintf(msg, msgcap, "the store answered HTTP %d", r.status);
        e = NEMA_ERR_NOENT;
    }
    if (e == NEMA_OK) {
        int n = pal_store_parse(m.buf, m.len, s_e, PAL_STORE_MAX);
        if (n < 0) {
            if (msg) snprintf(msg, msgcap, "%s is not a store list", s_base);
            e = NEMA_ERR_PROTO;
        } else {
            s_n = s_nfw = 0;
            for (int i = 0; i < n; i++) {
                if (strncmp(s_e[i].id, PAL_STORE_FW_PREFIX, sizeof(PAL_STORE_FW_PREFIX) - 1)) s_e[s_n++] = s_e[i];
                else if (s_nfw < sizeof(s_fw) / sizeof(s_fw[0])) s_fw[s_nfw++] = s_e[i];
            }
            uint8_t sum[32];
            ci_hash_t h;
            ci_hash_init(&h, CI_HASH_SHA256);
            ci_hash_update(&h, m.buf, m.len);
            ci_hash_final(&h, sum);
            s_signed = list_signed(sum);
        }
    } else if (msg && !msg[0]) {
        snprintf(msg, msgcap, "%s", e == NEMA_ERR_AUTH ? "the store's certificate is not trusted" : e == NEMA_ERR_UNSUPPORTED ? "no TLS in this build" : "cannot reach the store");
    }
    nema_free(m.buf);
    return e;
}

size_t pal_store_count(void) { return s_n; }

bool pal_store_get(size_t i, pal_store_entry_t *out) {
    if (i >= s_n) return false;
    *out = s_e[i];
    return true;
}

bool pal_store_find(const char *id, pal_store_entry_t *out) {
    for (size_t i = 0; i < s_n; i++)
        if (!strcmp(s_e[i].id, id)) {
            if (out) *out = s_e[i];
            return true;
        }
    return false;
}

void pal_store_installed(const char *id, char *version, size_t cap) {
    pal_reg_app_t a;
    version[0] = 0;
    if (psys_reg_find(id, &a) && a.source == PAL_SRC_INSTALLED) nema_strlcpy(version, a.version, cap);
}

/* A path in the list is relative to the list's URL, or a full URL. */
static void entry_url(const pal_store_entry_t *e, char *url, size_t cap) {
    if (!strncmp(e->path, "http://", 7) || !strncmp(e->path, "https://", 8)) nema_strlcpy(url, e->path, cap);
    else {
        const char *slash = strrchr(s_base, '/');
        snprintf(url, cap, "%.*s/%s", (int)(slash ? slash - s_base : 0), s_base, e->path);
    }
}

typedef struct {
    nema_obj_t *f;
    ci_hash_t h;
    uint32_t done, total;
    void (*progress)(uint32_t, uint32_t, void *);
    void *ctx;
} dl_t;

static int to_file(void *ctx, const void *b, size_t n) {
    dl_t *d = (dl_t *)ctx;
    if (nema_obj_write(d->f, b, n, 5000) != (int)n) return -1;
    ci_hash_update(&d->h, b, n);
    d->done += (uint32_t)n;
    if (d->progress) d->progress(d->done, d->total, d->ctx);
    return 0;
}

nema_err_t pal_store_install(const char *id, void (*progress)(uint32_t, uint32_t, void *), void *ctx, char *msg, size_t msgcap) {
    if (msg && msgcap) msg[0] = 0;
    pal_store_entry_t e;
    if (!pal_store_find(id, &e)) {
        if (msg) snprintf(msg, msgcap, "%s is not in the catalogue (refresh it?)", id);
        return NEMA_ERR_NOENT;
    }
    char url[PAL_HTTP_URL_MAX];
    entry_url(&e, url, sizeof(url));
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/store-%s.papp", e.id);
    dl_t *d = nema_calloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*d));
    if (!d) return NEMA_ERR_NOMEM;
    d->total = e.size, d->progress = progress, d->ctx = ctx;
    nema_err_t r = nema_fs_open(tmp, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &d->f);
    if (r == NEMA_OK) {
        ci_hash_init(&d->h, CI_HASH_SHA256);
        pal_http_req_t q = {.url = url, .redirects = 5, .timeout_ms = 20000, .on_data = to_file, .ctx = d};
        pal_http_resp_t resp;
        r = pal_http_request(&q, &resp);
        nema_obj_unref(d->f);
        uint8_t sum[32];
        ci_hash_final(&d->h, sum);
        if (r == NEMA_OK && resp.status != 200) {
            if (msg) snprintf(msg, msgcap, "download answered HTTP %d", resp.status);
            r = NEMA_ERR_NOENT;
        } else if (r == NEMA_OK && memcmp(sum, e.sha256, 32) != 0) {
            if (msg) snprintf(msg, msgcap, "the download does not match the catalogue's checksum");
            r = NEMA_ERR_AUTH;
        } else if (r != NEMA_OK && msg) {
            snprintf(msg, msgcap, "download failed (%d)", (int)r);
        }
        if (r == NEMA_OK) {
            char got[32];
            r = pal_papp_install_msg(tmp, got, sizeof(got), msg, msgcap);
        }
        (void)nema_fs_unlink(tmp);
    } else if (msg) {
        snprintf(msg, msgcap, "cannot write to /tmp");
    }
    nema_free(d);
    return r;
}

/* ---- the firmware ---- */

bool pal_store_signed(void) { return s_signed; }

bool pal_store_firmware(pal_store_entry_t *out) {
    char id[24];
    const char *board = pal_board_info()->name;
    snprintf(id, sizeof(id), PAL_STORE_FW_PREFIX "%s", board ? board : "");
    for (size_t i = 0; i < s_nfw; i++)
        if (!strcmp(s_fw[i].id, id)) {
            if (out) *out = s_fw[i];
            return true;
        }
    return false;
}

int pal_store_version_cmp(const char *a, const char *b) {
    for (;;) {
        char *ea, *eb;
        unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        a = ea, b = eb;
        if (*a == '.' && *b == '.') {
            a++, b++;
            continue;
        }
        if (*a == '.') return 1; /* "1.2.1" after "1.2" */
        if (*b == '.') return -1;
        int c = strcmp(a, b); /* the same numbers: a suffix decides, or they are equal */
        return c < 0 ? -1 : c > 0;
    }
}

typedef struct {
    const pal_ota_ops_t *o;
    ci_hash_t h;
    uint32_t done, total;
    nema_err_t err;
    void (*progress)(uint32_t, uint32_t, void *);
    void *ctx;
} fw_dl_t;

static int to_slot(void *ctx, const void *b, size_t n) {
    fw_dl_t *d = (fw_dl_t *)ctx;
    if (d->done + n > d->total) { /* more than the list said: not the image it describes */
        d->err = NEMA_ERR_AUTH;
        return -1;
    }
    d->err = d->o->write(b, n);
    if (d->err != NEMA_OK) return -1;
    ci_hash_update(&d->h, b, n);
    d->done += (uint32_t)n;
    if (d->progress) d->progress(d->done, d->total, d->ctx);
    return 0;
}

nema_err_t pal_store_update_firmware(void (*progress)(uint32_t, uint32_t, void *), void *ctx, char *msg, size_t msgcap) {
    if (msg && msgcap) msg[0] = 0;
    const pal_ota_ops_t *o = pal_ota();
    pal_store_entry_t e;
    if (!o) {
        if (msg) snprintf(msg, msgcap, "this device cannot update itself");
        return NEMA_ERR_UNSUPPORTED;
    }
    if (!pal_store_firmware(&e)) {
        if (msg) snprintf(msg, msgcap, "the catalogue has no firmware for this board");
        return NEMA_ERR_NOENT;
    }
    if (!s_signed && !psys_dev_mode()) { /* a firmware image runs with every right: only from a list a trusted key signed */
        if (msg) snprintf(msg, msgcap, "the catalogue is not signed by a trusted key (developer mode allows it)");
        return NEMA_ERR_PERM;
    }
    char url[PAL_HTTP_URL_MAX];
    entry_url(&e, url, sizeof(url));
    fw_dl_t *d = nema_calloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*d));
    if (!d) return NEMA_ERR_NOMEM;
    d->o = o, d->total = e.size, d->progress = progress, d->ctx = ctx;
    nema_err_t r = o->begin(e.size);
    if (r != NEMA_OK) {
        if (msg) snprintf(msg, msgcap, "cannot prepare the update slot (%d)", (int)r);
        nema_free(d);
        return r;
    }
    ci_hash_init(&d->h, CI_HASH_SHA256);
    pal_http_req_t q = {.url = url, .redirects = 5, .timeout_ms = 30000, .on_data = to_slot, .ctx = d};
    pal_http_resp_t resp;
    r = pal_http_request(&q, &resp);
    uint8_t sum[32];
    ci_hash_final(&d->h, sum);
    if (d->err != NEMA_OK) r = d->err;
    if (r == NEMA_OK && resp.status != 200) {
        if (msg) snprintf(msg, msgcap, "download answered HTTP %d", resp.status);
        r = NEMA_ERR_NOENT;
    } else if (r == NEMA_OK && (d->done != e.size || memcmp(sum, e.sha256, 32) != 0)) {
        if (msg) snprintf(msg, msgcap, "the download does not match the catalogue's checksum");
        r = NEMA_ERR_AUTH;
    } else if (r != NEMA_OK && msg && !msg[0]) {
        snprintf(msg, msgcap, r == NEMA_ERR_AUTH ? "the download is larger than the catalogue says" : "download failed (%d)", (int)r);
    }
    if (r == NEMA_OK) {
        r = o->end(); /* the platform checks the image too, and makes it the next boot */
        if (r != NEMA_OK && msg) snprintf(msg, msgcap, "the image was refused (%d)", (int)r);
    } else {
        o->abort();
    }
    nema_free(d);
    return r;
}
