/* Paired-client records, the device identifier, and the link config (link_clients.h). */
#include <stdio.h>
#include <string.h>

#include "../../third_party/monocypher/monocypher.h"
#include "crypto_prim.h"
#include "keystore.h"
#include "link_clients.h"
#include "nema/log.h"
#include "nema/mem.h"
#include "nema/port.h"
#include "nema/types.h"
#include "nema/vfs.h"
#include "pal/time.h"

#define DIR_LINK "/data/link"
#define DIR_CLIENTS "/data/link/clients"
#define PATH_DEVICE_ID "/data/link/device_id"
#define PATH_CONFIG "/data/link/config"
#define REC_MAGIC "PLC1"
#define NONCE_LEN 12
#define TAG_LEN 16
#define PLAIN_LEN (8 + (LC_NAME_MAX + 1) + 32 + 4 + 4) /* id, name (NUL-padded), token, created, last used */
#define REC_LEN (4 + NONCE_LEN + PLAIN_LEN + TAG_LEN)
#define PATH_MAX_LC 96

/* The number of records, kept: listing the folder costs a lookup per record on the board's flash (half a second for eight),
 * and screens show it on every redraw. Storing or removing a record forgets it (-1). */
static int s_count = -1;

static const char HEX[] = "0123456789abcdef";

void lc_hex_enc(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = HEX[b[i] >> 4];
        out[2 * i + 1] = HEX[b[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int lc_hex_dec(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != 2 * n) return 0;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 1;
}

static int ensure_dir(const char *p) {
    nema_stat_t st;
    if (nema_fs_stat(p, &st) == NEMA_OK) return NEMA_OK;
    nema_err_t e = nema_fs_mkdir(p);
    return e == NEMA_OK || e == NEMA_ERR_EXIST ? NEMA_OK : e;
}

static int ensure_dirs(void) {
    int rc = ensure_dir(DIR_LINK);
    return rc == NEMA_OK ? ensure_dir(DIR_CLIENTS) : rc;
}

static int read_all(const char *path, uint8_t *buf, size_t cap, size_t *got) {
    nema_obj_t *f;
    nema_err_t e = nema_fs_open(path, NEMA_O_READ, &f);
    if (e != NEMA_OK) return e;
    size_t n = 0;
    while (n < cap) {
        int r = nema_obj_read(f, buf + n, cap - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    nema_obj_unref(f);
    *got = n;
    return NEMA_OK;
}

static int write_all(const char *path, const uint8_t *buf, size_t n) {
    nema_obj_t *f;
    nema_err_t e = nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f);
    if (e != NEMA_OK) return e;
    int w = nema_obj_write(f, buf, n, -1);
    nema_obj_unref(f);
    return w == (int)n ? NEMA_OK : NEMA_ERR_IO;
}

/* Writes under a temporary name and renames, so a power cut leaves the old record or the new one. */
static int write_atomic(const char *path, const uint8_t *buf, size_t n) {
    char tmp[PATH_MAX_LC];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return NEMA_ERR_INVAL;
    int rc = write_all(tmp, buf, n);
    if (rc == NEMA_OK) rc = nema_fs_rename(tmp, path);
    if (rc != NEMA_OK) nema_fs_unlink(tmp);
    return rc;
}

static int rec_path(char *out, size_t cap, const uint8_t id[8]) {
    char hex[17];
    lc_hex_enc(id, 8, hex);
    int n = snprintf(out, cap, "%s/%s.cli", DIR_CLIENTS, hex);
    return n > 0 && (size_t)n < cap ? NEMA_OK : NEMA_ERR_INVAL;
}

/* The associated data binds a record to its client id. */
static size_t rec_aad(uint8_t *aad, const uint8_t id[8]) {
    memcpy(aad, REC_MAGIC, 4);
    memcpy(aad + 4, id, 8);
    return 12;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int lc_load(const uint8_t id[8], lc_rec_t *r) {
    char path[PATH_MAX_LC];
    uint8_t file[REC_LEN], plain[PLAIN_LEN], dk[32], aad[12];
    size_t got = 0;
    int rc = rec_path(path, sizeof(path), id);
    if (rc == NEMA_OK) rc = read_all(path, file, sizeof(file), &got);
    if (rc != NEMA_OK) return rc;
    if (got != sizeof(file) || memcmp(file, REC_MAGIC, 4) != 0) return NEMA_ERR_AUTH;
    rc = psys_keystore_device_key(dk);
    if (rc != NEMA_OK) return rc;
    memcpy(plain, file + 4 + NONCE_LEN, PLAIN_LEN);
    rec_aad(aad, id);
    rc = prim_aead_open(dk, file + 4, aad, sizeof(aad), plain, PLAIN_LEN, file + 4 + NONCE_LEN + PLAIN_LEN);
    crypto_wipe(dk, sizeof(dk));
    if (rc != NEMA_OK) {
        crypto_wipe(plain, sizeof(plain));
        return NEMA_ERR_AUTH;
    }
    memcpy(r->id, plain, 8);
    memcpy(r->name, plain + 8, LC_NAME_MAX + 1);
    r->name[LC_NAME_MAX] = 0;
    memcpy(r->token, plain + 8 + LC_NAME_MAX + 1, 32);
    r->created = get_u32(plain + 8 + LC_NAME_MAX + 1 + 32);
    r->last_used = get_u32(plain + 8 + LC_NAME_MAX + 1 + 32 + 4);
    crypto_wipe(plain, sizeof(plain));
    return memcmp(r->id, id, 8) == 0 ? NEMA_OK : NEMA_ERR_AUTH;
}

int lc_store(const lc_rec_t *r) {
    char path[PATH_MAX_LC];
    uint8_t file[REC_LEN], plain[PLAIN_LEN], dk[32], aad[12];
    int rc = ensure_dirs();
    if (rc != NEMA_OK) return rc;
    rc = rec_path(path, sizeof(path), r->id);
    if (rc != NEMA_OK) return rc;
    rc = psys_keystore_device_key(dk);
    if (rc != NEMA_OK) return rc;
    memset(plain, 0, sizeof(plain));
    memcpy(plain, r->id, 8);
    memcpy(plain + 8, r->name, LC_NAME_MAX);
    memcpy(plain + 8 + LC_NAME_MAX + 1, r->token, 32);
    put_u32(plain + 8 + LC_NAME_MAX + 1 + 32, r->created);
    put_u32(plain + 8 + LC_NAME_MAX + 1 + 32 + 4, r->last_used);
    memcpy(file, REC_MAGIC, 4);
    nema_port_random(file + 4, NONCE_LEN);
    rec_aad(aad, r->id);
    memcpy(file + 4 + NONCE_LEN, plain, PLAIN_LEN);
    prim_aead_seal(dk, file + 4, aad, sizeof(aad), file + 4 + NONCE_LEN, PLAIN_LEN, file + 4 + NONCE_LEN + PLAIN_LEN);
    crypto_wipe(dk, sizeof(dk));
    crypto_wipe(plain, sizeof(plain));
    rc = write_atomic(path, file, sizeof(file));
    s_count = -1;
    return rc;
}

int lc_remove(const uint8_t id[8]) {
    char path[PATH_MAX_LC];
    int rc = rec_path(path, sizeof(path), id);
    if (rc != NEMA_OK) return rc;
    nema_stat_t st;
    char tmp[PATH_MAX_LC + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    (void)nema_fs_unlink(tmp); /* a write that a reset cut short leaves this behind */
    if (nema_fs_stat(path, &st) != NEMA_OK) return NEMA_ERR_NOENT;
    s_count = -1;
    return nema_fs_unlink(path);
}

/* Calls fn for each record file (name without the .cli suffix); fn returns 1 to stop. */
static int each_record(int (*fn)(const char *hex, void *arg), void *arg) {
    nema_dirent_t ent;
    int n = 0;
    for (size_t i = 0; nema_fs_readdir(DIR_CLIENTS, i, &ent) == NEMA_OK; i++) {
        size_t len = strlen(ent.name);
        if (len != 16 + 4 || strcmp(ent.name + 16, ".cli") != 0) continue;
        n++;
        if (fn && fn(ent.name, arg)) break;
    }
    return n;
}

static int count_one(const char *hex, void *arg) {
    (void)hex, (void)arg;
    return 0;
}

int lc_count(void) {
    if (s_count >= 0) return s_count;
    nema_stat_t st;
    if (nema_fs_stat(DIR_CLIENTS, &st) != NEMA_OK) return s_count = 0;
    return s_count = each_record(count_one, NULL);
}

typedef struct {
    uint8_t (*ids)[8];
    int cap, n;
} ids_arg_t;

/* The entry names are "<16 hex>.cli": the id is the first 16 characters. */
static int collect_one(const char *name, void *arg) {
    ids_arg_t *a = arg;
    char hex[17];
    if (a->n >= a->cap) return 1;
    memcpy(hex, name, 16);
    hex[16] = 0;
    if (lc_hex_dec(hex, a->ids[a->n], 8)) a->n++;
    return 0;
}

int lc_ids(uint8_t ids[][8], int cap) {
    ids_arg_t a = {ids, cap, 0};
    nema_stat_t st;
    if (nema_fs_stat(DIR_CLIENTS, &st) != NEMA_OK) return 0;
    each_record(collect_one, &a);
    return a.n;
}

typedef struct {
    char *out;
    size_t cap, used;
} text_arg_t;

static int text_one(const char *name, void *arg) {
    text_arg_t *a = arg;
    uint8_t id[8];
    lc_rec_t r;
    char hex[17];
    memcpy(hex, name, 16);
    hex[16] = 0;
    if (!lc_hex_dec(hex, id, 8) || lc_load(id, &r) != NEMA_OK) return 0;
    int n = snprintf(a->out + a->used, a->cap - a->used, "%s  %-24s created %lu  last used %lu\n", hex, r.name,
                     (unsigned long)r.created, (unsigned long)r.last_used);
    if (n > 0 && (size_t)n < a->cap - a->used) a->used += (size_t)n;
    return 0;
}

size_t lc_list_text(char *out, size_t cap) {
    text_arg_t a = {out, cap, 0};
    out[0] = 0;
    nema_stat_t st;
    if (nema_fs_stat(DIR_CLIENTS, &st) != NEMA_OK) return 0;
    each_record(text_one, &a);
    return a.used;
}

int lc_device_id(char out[17]) {
    uint8_t id[8];
    size_t got = 0;
    int rc = ensure_dirs();
    if (rc != NEMA_OK) return rc;
    char hex[17];
    rc = read_all(PATH_DEVICE_ID, (uint8_t *)hex, 16, &got);
    hex[got < 16 ? got : 16] = 0; /* lc_hex_dec measures the text: without this it read past the 16 digits, and on the board the
                                   * stack there was not zero, so every hello made a new id and no browser found its pairing */
    if (rc == NEMA_OK && got == 16 && lc_hex_dec(hex, id, 8)) {
        memcpy(out, hex, 16);
        out[16] = 0;
        return NEMA_OK;
    }
    NEMA_LOGW("link", "device id: read %d, %u bytes: making a new one", rc, (unsigned)got);
    nema_port_random(id, sizeof(id));
    lc_hex_enc(id, 8, out);
    rc = write_atomic(PATH_DEVICE_ID, (const uint8_t *)out, 16);
    if (rc != NEMA_OK) NEMA_LOGW("link", "device id: cannot keep it (%d)", rc);
    return rc;
}

/* The config file: key=value lines, as the settings file. */
int lc_config_get(const char *key, char *out, size_t cap) {
    char buf[256];
    size_t got = 0;
    if (read_all(PATH_CONFIG, (uint8_t *)buf, sizeof(buf) - 1, &got) != NEMA_OK) return NEMA_ERR_NOENT;
    buf[got] = 0;
    size_t kn = strlen(key);
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, key, kn) == 0 && line[kn] == '=') {
            nema_strlcpy(out, line + kn + 1, cap);
            return (int)strlen(out);
        }
        line = nl ? nl + 1 : NULL;
    }
    return NEMA_ERR_NOENT;
}

int lc_config_set(const char *key, const char *value) {
    char buf[256] = "", next[256];
    size_t got = 0;
    int rc = ensure_dirs();
    if (rc != NEMA_OK) return rc;
    if (read_all(PATH_CONFIG, (uint8_t *)buf, sizeof(buf) - 1, &got) == NEMA_OK) buf[got] = 0;
    size_t kn = strlen(key), used = 0;
    int done = 0;
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, key, kn) == 0 && line[kn] == '=') {
            used += (size_t)snprintf(next + used, sizeof(next) - used, "%s=%s\n", key, value);
            done = 1;
        } else if (line[0]) {
            used += (size_t)snprintf(next + used, sizeof(next) - used, "%s\n", line);
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!done) used += (size_t)snprintf(next + used, sizeof(next) - used, "%s=%s\n", key, value);
    if (used >= sizeof(next)) return NEMA_ERR_RANGE;
    return write_atomic(PATH_CONFIG, (const uint8_t *)next, used);
}
