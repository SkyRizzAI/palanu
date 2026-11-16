/* The kv module of the palanu API (api/palanu/kv.h): a key-value store per app, in /apps/<id>/kv.dat.
 *
 * The file is a log of records, appended as they are made. A record is
 *   "KV" op(1) klen(1) vlen(2, little-endian) key value crc32(4, little-endian, over every byte before it)
 * op 1 sets key to value; op 2 deletes key (vlen 0). The live value of a key is its last valid record. A record that is
 * cut short, or fails its CRC, ends the log: that is a write torn by a power cut. Reads ignore it, and the next write
 * rewrites the file from the live records. A write also rewrites the file when more than half of its records are dead.
 * The rewrite goes to kv.tmp and is renamed over kv.dat, so a power cut leaves one of the two whole files.
 * Every operation scans the log from its start: at most 64 keys, so the scan is short. One mutex serialises the store
 * (all apps and the system share it), and the scan state below is only touched under that mutex. */
#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "../src/api_ctx.h"
#include "../src/api_impl.h"

#define KV_KEYS_MAX 64 /* PAL_KV_KEYS_MAX */
#define KV_KEY_MAX 32  /* PAL_KV_KEY_MAX */
#define KV_VAL_MAX 1024 /* PAL_KV_VALUE_MAX */
#define KV_HDR 6        /* "KV", op, klen, vlen(2) */
#define KV_REC_MAX (KV_HDR + KV_KEY_MAX + KV_VAL_MAX + 4)
#define OP_SET 1
#define OP_DEL 2

typedef struct {
    char key[KV_KEY_MAX + 1];
    uint32_t rec; /* offset of the live record in the log */
    uint32_t vlen;
} kv_ent_t;

static nema_port_mutex_t *s_lock;
static kv_ent_t s_idx[KV_KEYS_MAX]; /* the live keys, in the order they were first set */
static int s_nidx;
static uint32_t s_recs;   /* valid records in the last scan */
static uint32_t s_valid;  /* bytes of valid records in the last scan */
static uint32_t s_size;   /* size of the file in the last scan */
static uint8_t s_rec[KV_REC_MAX];    /* the record being read */
static uint8_t s_out[KV_REC_MAX];    /* the record being written */

/* ---- lock: made on first use; the race of two first uses keeps one mutex and frees the other ---- */

static void lock(void) {
    if (!__atomic_load_n(&s_lock, __ATOMIC_ACQUIRE)) {
        nema_port_mutex_t *m = nema_port_mutex_create();
        if (!m) nema_panic("kv: no mutex");
        nema_port_mutex_t *expect = NULL;
        if (!__atomic_compare_exchange_n(&s_lock, &expect, m, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            nema_port_mutex_destroy(m);
    }
    nema_port_mutex_lock(s_lock);
}

static void unlock(void) { nema_port_mutex_unlock(s_lock); }

/* ---- names and paths ---- */

static int key_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; }

static int key_ok(const char *k) {
    if (!k) return 0;
    size_t n = strlen(k);
    if (n == 0 || n > KV_KEY_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (!key_char(k[i])) return 0;
    return 1;
}

/* The app's id, or "system" for native callers (no context). */
static const char *owner_of(api_ctx_t *c) { return c ? api_app_id(c) : "system"; }

static int paths_of(api_ctx_t *c, char *file, char *tmp, size_t cap) {
    int a = snprintf(file, cap, "/apps/%s/kv.dat", owner_of(c));
    int b = snprintf(tmp, cap, "/apps/%s/kv.tmp", owner_of(c));
    return a > 0 && (size_t)a < cap && b > 0 && (size_t)b < cap ? NEMA_OK : NEMA_ERR_RANGE;
}

static int32_t ensure_dir(const char *dir) {
    nema_stat_t st;
    if (nema_fs_stat(dir, &st) == NEMA_OK) return NEMA_OK;
    nema_err_t e = nema_fs_mkdir(dir);
    return e == NEMA_OK || e == NEMA_ERR_EXIST ? NEMA_OK : e;
}

static int32_t ensure_owner_dir(api_ctx_t *c) {
    char dir[64];
    if (snprintf(dir, sizeof(dir), "/apps/%s", owner_of(c)) >= (int)sizeof(dir)) return NEMA_ERR_RANGE;
    int32_t r = ensure_dir("/apps");
    return r != NEMA_OK ? r : ensure_dir(dir);
}

/* ---- records ---- */

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* Writes the record of op for key (and val, vlen bytes) into out; returns its length. */
static uint32_t encode(uint8_t *out, int op, const char *key, const uint8_t *val, uint32_t vlen) {
    uint32_t kl = (uint32_t)strlen(key);
    out[0] = 'K';
    out[1] = 'V';
    out[2] = (uint8_t)op;
    out[3] = (uint8_t)kl;
    api_put16(out + 4, vlen);
    memcpy(out + KV_HDR, key, kl);
    if (vlen) memcpy(out + KV_HDR + kl, val, vlen);
    uint32_t n = KV_HDR + kl + vlen;
    api_put32(out + n, crc32(out, n));
    return n + 4;
}

static size_t read_full(nema_obj_t *f, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = nema_obj_read(f, (uint8_t *)buf + got, n - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return got;
}

/* Reads the record at the file position into s_rec. Returns its length, or 0 at the end of the valid log (also for a
 * torn or corrupt record). */
static uint32_t read_record(nema_obj_t *f, int *op, uint32_t *klen, uint32_t *vlen) {
    if (read_full(f, s_rec, KV_HDR) < KV_HDR) return 0;
    if (s_rec[0] != 'K' || s_rec[1] != 'V') return 0;
    int o = s_rec[2];
    uint32_t kl = s_rec[3], vl = api_get16(s_rec + 4);
    if ((o != OP_SET && o != OP_DEL) || kl < 1 || kl > KV_KEY_MAX || vl > KV_VAL_MAX) return 0;
    if (o == OP_DEL && vl != 0) return 0;
    uint32_t body = kl + vl + 4;
    if (read_full(f, s_rec + KV_HDR, body) < body) return 0;
    uint32_t n = KV_HDR + kl + vl;
    if (crc32(s_rec, n) != api_get32(s_rec + n)) return 0;
    for (uint32_t i = 0; i < kl; i++)
        if (!key_char(s_rec[KV_HDR + i])) return 0;
    *op = o;
    *klen = kl;
    *vlen = vl;
    return n + 4;
}

/* ---- the index: built by a scan of the log ---- */

static int idx_find(const char *key) {
    for (int i = 0; i < s_nidx; i++)
        if (strcmp(s_idx[i].key, key) == 0) return i;
    return -1;
}

static void idx_remove(int i) {
    s_idx[i] = s_idx[--s_nidx];
}

/* What a scan wants with a key: the live value of want is copied to out when it fits (found, vlen, fits). */
typedef struct {
    const char *want;
    uint8_t *out;
    uint32_t cap;
    int found;
    uint32_t vlen;
    int fits;
} want_t;

/* Scans the log at path into the index and the counters. A missing file is an empty log. */
static void scan(const char *path, want_t *w) {
    s_nidx = 0;
    s_recs = 0;
    s_valid = 0;
    s_size = 0;
    if (w) w->found = 0, w->fits = 0, w->vlen = 0;
    nema_stat_t st;
    if (nema_fs_stat(path, &st) != NEMA_OK) return;
    s_size = st.size;
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_READ, &f) != NEMA_OK) return;
    uint32_t off = 0;
    for (;;) {
        int op;
        uint32_t kl, vl;
        uint32_t n = read_record(f, &op, &kl, &vl);
        if (n == 0) break;
        char key[KV_KEY_MAX + 1];
        memcpy(key, s_rec + KV_HDR, kl);
        key[kl] = 0;
        int i = idx_find(key);
        if (op == OP_SET) {
            if (i < 0) {
                if (s_nidx == KV_KEYS_MAX) break; /* more keys than the store allows: not written by this code */
                i = s_nidx++;
                memcpy(s_idx[i].key, key, kl + 1);
            }
            s_idx[i].rec = off;
            s_idx[i].vlen = vl;
        } else if (i >= 0) {
            idx_remove(i);
        }
        if (w && w->want && strcmp(w->want, key) == 0) {
            w->found = op == OP_SET;
            w->vlen = vl;
            w->fits = op == OP_SET && vl <= w->cap;
            if (w->fits && vl) memcpy(w->out, s_rec + KV_HDR + kl, vl);
        }
        off += n;
        s_recs++;
    }
    s_valid = off;
    nema_obj_unref(f);
}

static int torn(void) { return s_valid != s_size; }

/* ---- writing ---- */

static int32_t write_all(nema_obj_t *f, const void *buf, uint32_t n) {
    int w = nema_obj_write(f, buf, n, -1);
    return w == (int)n ? NEMA_OK : NEMA_ERR_IO;
}

/* Appends one record to the log. */
static int32_t append(const char *path, const uint8_t *rec, uint32_t n) {
    nema_obj_t *f;
    nema_err_t e = nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_APPEND, &f);
    if (e != NEMA_OK) return e;
    int32_t r = write_all(f, rec, n);
    nema_obj_unref(f);
    return r;
}

/* Rewrites the log: the live records (except the one of drop), then the extra record when there is one. The file is
 * replaced by a rename, so the old log stays whole until the new one is complete. Uses the index of the last scan. */
static int32_t compact(const char *path, const char *tmp, const char *drop, const uint8_t *extra, uint32_t extra_n) {
    nema_obj_t *out;
    nema_err_t e = nema_fs_open(tmp, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &out);
    if (e != NEMA_OK) return e;
    int32_t r = NEMA_OK;
    nema_obj_t *in = NULL;
    if (s_size && nema_fs_open(path, NEMA_O_READ, &in) != NEMA_OK) r = NEMA_ERR_IO;
    uint32_t off = 0;
    while (r == NEMA_OK && in && off < s_valid) {
        int op;
        uint32_t kl, vl;
        uint32_t n = read_record(in, &op, &kl, &vl);
        if (n == 0) break;
        for (int i = 0; i < s_nidx; i++) {
            if (s_idx[i].rec != off || strcmp(s_idx[i].key, drop) == 0) continue;
            r = write_all(out, s_rec, n);
            break;
        }
        off += n;
    }
    if (in) nema_obj_unref(in);
    if (r == NEMA_OK && extra_n) r = write_all(out, extra, extra_n);
    nema_obj_unref(out);
    if (r == NEMA_OK) r = nema_fs_rename(tmp, path);
    if (r != NEMA_OK) nema_fs_unlink(tmp);
    return r;
}

/* Decides between appending and compacting, after the scan, for an op that adds one record and drops at most one key. */
static int32_t commit(const char *path, const char *tmp, const char *key, int op, const uint8_t *rec, uint32_t n,
                      int live_delta) {
    uint32_t total = s_recs + 1;
    int64_t live = (int64_t)s_nidx + live_delta;
    int64_t dead = (int64_t)total - live;
    if (torn() || 2 * dead > (int64_t)total) {
        /* a delete is not kept in the rewritten log: the key simply is not copied */
        return op == OP_SET ? compact(path, tmp, key, rec, n) : compact(path, tmp, key, NULL, 0);
    }
    return append(path, rec, n);
}

/* ---- the calls ---- */

int32_t psys_api_kv_get(api_ctx_t *c, const char *key, void *buf, uint32_t cap) {
    if (!key_ok(key)) return NEMA_ERR_INVAL;
    char file[64], tmp[64];
    if (paths_of(c, file, tmp, sizeof(file)) != NEMA_OK) return NEMA_ERR_RANGE;
    lock();
    want_t w = {.want = key, .out = buf, .cap = cap};
    scan(file, &w);
    unlock();
    if (!w.found) return NEMA_ERR_NOENT;
    if (!w.fits) return NEMA_ERR_RANGE;
    return (int32_t)w.vlen;
}

int32_t psys_api_kv_set(api_ctx_t *c, const char *key, const void *val, uint32_t len) {
    if (!key_ok(key) || len > KV_VAL_MAX || (len && !val)) return NEMA_ERR_INVAL;
    char file[64], tmp[64];
    if (paths_of(c, file, tmp, sizeof(file)) != NEMA_OK) return NEMA_ERR_RANGE;
    lock();
    int32_t r = ensure_owner_dir(c);
    if (r == NEMA_OK) {
        scan(file, NULL);
        int exists = idx_find(key) >= 0;
        if (!exists && s_nidx >= KV_KEYS_MAX) {
            r = NEMA_ERR_NOSPC;
        } else {
            uint32_t n = encode(s_out, OP_SET, key, val, len);
            r = commit(file, tmp, key, OP_SET, s_out, n, exists ? 0 : 1);
        }
    }
    unlock();
    return r;
}

int32_t psys_api_kv_del(api_ctx_t *c, const char *key) {
    if (!key_ok(key)) return NEMA_ERR_INVAL;
    char file[64], tmp[64];
    if (paths_of(c, file, tmp, sizeof(file)) != NEMA_OK) return NEMA_ERR_RANGE;
    lock();
    int32_t r;
    scan(file, NULL);
    if (idx_find(key) < 0) {
        r = NEMA_ERR_NOENT;
    } else {
        uint32_t n = encode(s_out, OP_DEL, key, NULL, 0);
        r = commit(file, tmp, key, OP_DEL, s_out, n, -1);
    }
    unlock();
    return r;
}

int32_t psys_api_kv_keys(api_ctx_t *c, void *out, uint32_t cap) {
    char file[64], tmp[64];
    if (paths_of(c, file, tmp, sizeof(file)) != NEMA_OK) return NEMA_ERR_RANGE;
    lock();
    scan(file, NULL);
    uint32_t total = 0;
    for (int i = 0; i < s_nidx; i++) total += (uint32_t)strlen(s_idx[i].key) + 1;
    int32_t r = total;
    if (total > cap) {
        r = NEMA_ERR_RANGE;
    } else {
        char *p = (char *)out;
        for (int i = 0; i < s_nidx; i++) {
            size_t kl = strlen(s_idx[i].key);
            memcpy(p, s_idx[i].key, kl);
            p[kl] = '\n';
            p += kl + 1;
        }
    }
    unlock();
    return r;
}
