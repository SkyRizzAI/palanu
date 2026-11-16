/* Keystore storage (keystore.h). Files are written under a temporary name and renamed, so a power cut leaves the old key
 * or the new one, never half of one. */
#include <stdio.h>
#include <string.h>

#include "../../third_party/monocypher/monocypher.h"
#include "crypto_mac.h"
#include "crypto_prim.h"
#include "keystore.h"
#include "nema/mem.h"
#include "nema/port.h"
#include "nema/types.h"
#include "nema/vfs.h"

#define MAGIC "PKS1"
#define NONCE_LEN 12
#define TAG_LEN 16
/* magic(4) type(1) nonce(12) ciphertext(KS_PLAIN_LEN) tag(16) */
#define REC_LEN (4 + 1 + NONCE_LEN + KS_PLAIN_LEN + TAG_LEN)
#define PATH_MAX_KS 96

static const char LABEL[] = "palanu keystore device key v1";

static int read_file(const char *path, uint8_t *buf, size_t cap, size_t *got) {
    nema_obj_t *f;
    nema_err_t e = nema_fs_open(path, NEMA_O_READ, &f);
    if (e != NEMA_OK) return e;
    size_t n = 0;
    while (n < cap) {
        int r = nema_obj_read(f, buf + n, cap - n, 0);
        if (r < 0) break;
        if (r == 0) break;
        n += (size_t)r;
    }
    nema_obj_unref(f);
    *got = n;
    return NEMA_OK;
}

static int write_file(const char *path, const uint8_t *buf, size_t n) {
    nema_obj_t *f;
    nema_err_t e = nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f);
    if (e != NEMA_OK) return e;
    int w = nema_obj_write(f, buf, n, -1);
    nema_obj_unref(f);
    return w == (int)n ? NEMA_OK : NEMA_ERR_IO;
}

static int ensure_dir(void) {
    nema_stat_t st;
    if (nema_fs_stat(KS_DIR, &st) == NEMA_OK) return NEMA_OK;
    nema_err_t e = nema_fs_mkdir(KS_DIR);
    return e == NEMA_OK || e == NEMA_ERR_EXIST ? NEMA_OK : e;
}

static int key_path(char *out, size_t cap, const char *name) {
    int n = snprintf(out, cap, "%s/%s.key", KS_DIR, name);
    return n > 0 && (size_t)n < cap ? NEMA_OK : NEMA_ERR_INVAL;
}

/* True when the directory holds at least one stored key. */
static int any_key(void) {
    nema_dirent_t ent;
    for (size_t i = 0; nema_fs_readdir(KS_DIR, i, &ent) == NEMA_OK; i++) {
        size_t n = strlen(ent.name);
        if (n > 4 && strcmp(ent.name + n - 4, ".key") == 0) return 1;
    }
    return 0;
}

int psys_keystore_device_key(uint8_t out[32]) {
    uint8_t secret[32];
    size_t got = 0;
    int rc = ensure_dir();
    if (rc != NEMA_OK) return rc;
    rc = read_file(KS_DIR "/.dk", secret, sizeof(secret), &got);
    if (rc != NEMA_OK && rc != NEMA_ERR_NOENT) return rc; /* no secret yet is the first-use case below */
    if (got != sizeof(secret)) {
        if (any_key()) return NEMA_ERR_IO; /* keys exist but their secret is gone: they cannot be opened, so do not guess */
        nema_port_random(secret, sizeof(secret));
        rc = write_file(KS_DIR "/.dk", secret, sizeof(secret));
        if (rc != NEMA_OK) {
            crypto_wipe(secret, sizeof(secret));
            return rc;
        }
    }
    crypto_mac_t m;
    rc = crypto_mac_hmac(&m, CI_HASH_SHA256, secret, sizeof(secret));
    if (rc == NEMA_OK) {
        crypto_mac_update(&m, LABEL, sizeof(LABEL) - 1);
        crypto_mac_final(&m, out);
    }
    crypto_wipe(secret, sizeof(secret));
    return rc;
}

int ks_name_ok(const char *name, size_t n) {
    if (!name || n == 0 || n > KS_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

int ks_exists(const char *name) {
    char path[PATH_MAX_KS];
    nema_stat_t st;
    if (key_path(path, sizeof(path), name) != NEMA_OK) return 0;
    return nema_fs_stat(path, &st) == NEMA_OK;
}

/* The associated data: magic, type, name. Binds a record to its name and type. */
static size_t aad_of(uint8_t *aad, uint8_t type, const char *name) {
    memcpy(aad, MAGIC, 4);
    aad[4] = type;
    size_t n = strlen(name);
    memcpy(aad + 5, name, n);
    return 5 + n;
}

int ks_save(const char *name, uint8_t type, const uint8_t plain[KS_PLAIN_LEN]) {
    if (!ks_name_ok(name, strlen(name))) return NEMA_ERR_INVAL;
    uint8_t dk[32], rec[REC_LEN], aad[5 + KS_NAME_MAX];
    int rc = psys_keystore_device_key(dk);
    if (rc != NEMA_OK) return rc;
    memcpy(rec, MAGIC, 4);
    rec[4] = type;
    nema_port_random(rec + 5, NONCE_LEN);
    memcpy(rec + 5 + NONCE_LEN, plain, KS_PLAIN_LEN);
    size_t alen = aad_of(aad, type, name);
    prim_aead_seal(dk, rec + 5, aad, alen, rec + 5 + NONCE_LEN, KS_PLAIN_LEN, rec + 5 + NONCE_LEN + KS_PLAIN_LEN);
    crypto_wipe(dk, sizeof(dk));
    char path[PATH_MAX_KS], tmp[PATH_MAX_KS];
    rc = key_path(path, sizeof(path), name);
    if (rc == NEMA_OK) rc = snprintf(tmp, sizeof(tmp), "%s/%s.tmp", KS_DIR, name) > 0 ? NEMA_OK : NEMA_ERR_INVAL;
    if (rc == NEMA_OK) rc = write_file(tmp, rec, sizeof(rec));
    if (rc == NEMA_OK) rc = nema_fs_rename(tmp, path);
    if (rc != NEMA_OK) nema_fs_unlink(tmp);
    return rc;
}

int ks_load(const char *name, uint8_t *type, uint8_t plain[KS_PLAIN_LEN]) {
    if (!ks_name_ok(name, strlen(name))) return NEMA_ERR_INVAL;
    char path[PATH_MAX_KS];
    uint8_t rec[REC_LEN], dk[32], aad[5 + KS_NAME_MAX];
    size_t got = 0;
    int rc = key_path(path, sizeof(path), name);
    if (rc == NEMA_OK) rc = read_file(path, rec, sizeof(rec), &got);
    if (rc != NEMA_OK) return rc;
    if (got != sizeof(rec) || memcmp(rec, MAGIC, 4) != 0) return NEMA_ERR_IO;
    rc = psys_keystore_device_key(dk);
    if (rc != NEMA_OK) return rc;
    size_t alen = aad_of(aad, rec[4], name);
    uint8_t out[KS_PLAIN_LEN];
    memcpy(out, rec + 5 + NONCE_LEN, KS_PLAIN_LEN); /* decrypted in place in this copy */
    rc = prim_aead_open(dk, rec + 5, aad, alen, out, KS_PLAIN_LEN, rec + 5 + NONCE_LEN + KS_PLAIN_LEN);
    crypto_wipe(dk, sizeof(dk));
    if (rc != NEMA_OK) {
        crypto_wipe(out, sizeof(out));
        return rc; /* NEMA_ERR_AUTH: the file was altered, or belongs to another device */
    }
    memcpy(plain, out, KS_PLAIN_LEN);
    crypto_wipe(out, sizeof(out));
    *type = rec[4];
    return NEMA_OK;
}

int ks_delete(const char *name) {
    char path[PATH_MAX_KS];
    if (!ks_name_ok(name, strlen(name))) return NEMA_ERR_INVAL;
    int rc = key_path(path, sizeof(path), name);
    return rc == NEMA_OK ? nema_fs_unlink(path) : rc;
}

int ks_list(char *out, size_t cap) {
    size_t used = 0;
    nema_dirent_t ent;
    for (size_t i = 0; nema_fs_readdir(KS_DIR, i, &ent) == NEMA_OK; i++) {
        size_t n = strlen(ent.name);
        if (n <= 4 || strcmp(ent.name + n - 4, ".key") != 0) continue;
        n -= 4;
        if (used + n + 1 > cap) return NEMA_ERR_RANGE;
        memcpy(out + used, ent.name, n);
        out[used + n] = '\n';
        used += n + 1;
    }
    return (int)used;
}
