/* The keystore: generate, import, load, sign, list, delete, on a host directory mounted at "/". Checks that the
 * private bytes are not in the file, that a file cannot be moved under another name, and that a tampered file is
 * refused. The import uses the RFC 8032 section 7.1 test 1 key, so the signatures are known. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../system/src/api_impl.h"
#include "../../system/src/crypto_prim.h"
#include "../../system/src/keystore.h"
#include "nema/nema.h"
#include "test.h"

static char ROOT[256];
static api_ctx_t *NO_CTX = NULL;

static const char SEED1[] = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char PK1[] = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char SIG1[] =
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";

static size_t unhex(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex) / 2;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static int eq_hex(const uint8_t *buf, size_t n, const char *hex) {
    uint8_t want[128];
    size_t m = unhex(hex, want, sizeof(want));
    return m == n && memcmp(buf, want, n) == 0;
}

/* ---- building an OpenSSH key file for the import tests ---- */

static size_t put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
    return 4;
}

static size_t put_str(uint8_t *p, const void *s, uint32_t n) {
    put_u32(p, n);
    memcpy(p + 4, s, n);
    return 4 + n;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Writes the armored file for a binary blob into text (returns its length). */
static size_t armor(const uint8_t *bin, size_t n, char *text, size_t cap) {
    size_t t = (size_t)snprintf(text, cap, "-----BEGIN OPENSSH PRIVATE KEY-----\n");
    size_t col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)bin[i] << 16;
        if (i + 1 < n) v |= (uint32_t)bin[i + 1] << 8;
        if (i + 2 < n) v |= bin[i + 2];
        char q[4] = {B64[(v >> 18) & 63], B64[(v >> 12) & 63], i + 1 < n ? B64[(v >> 6) & 63] : '=',
                     i + 2 < n ? B64[v & 63] : '='};
        for (int j = 0; j < 4; j++) text[t++] = q[j];
        if (++col == 17) text[t++] = '\n', col = 0;
    }
    if (col) text[t++] = '\n';
    t += (size_t)snprintf(text + t, cap - t, "-----END OPENSSH PRIVATE KEY-----\n");
    return t;
}

/* An unencrypted ssh-ed25519 key with the given cipher name (use "none" for a real one). */
static size_t make_openssh(const char *cipher, char *text, size_t cap) {
    static uint8_t bin[1024];
    uint8_t seed[32], pk[32], sk[64];
    unhex(SEED1, seed, 32);
    unhex(PK1, pk, 32);
    prim_ed25519_keypair(sk, pk, seed);
    size_t n = 0;
    memcpy(bin, "openssh-key-v1", 15), bin[14] = 0, n = 15;
    n += put_str(bin + n, cipher, (uint32_t)strlen(cipher));
    n += put_str(bin + n, "none", 4);
    n += put_str(bin + n, "", 0);
    n += put_u32(bin + n, 1);
    uint8_t pubblob[64];
    size_t pl = put_str(pubblob, "ssh-ed25519", 11);
    pl += put_str(pubblob + pl, pk, 32);
    n += put_str(bin + n, pubblob, (uint32_t)pl);
    uint8_t priv[256];
    size_t q = 0;
    q += put_u32(priv, 0x12345678u);
    q += put_u32(priv + q, 0x12345678u);
    q += put_str(priv + q, "ssh-ed25519", 11);
    q += put_str(priv + q, pk, 32);
    q += put_str(priv + q, sk, 64);
    q += put_str(priv + q, "test", 4);
    for (uint8_t pad = 1; (q % 8) != 0; pad++) priv[q++] = pad;
    n += put_str(bin + n, priv, (uint32_t)q);
    return armor(bin, n, text, cap);
}

/* ---- the host directory and the paths inside it ---- */

static void path_of(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s/%s", ROOT, rel); }

static int read_all(const char *path, uint8_t *buf, size_t cap, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    *n = fread(buf, 1, cap, f);
    fclose(f);
    return 1;
}

static int write_all(const char *path, const uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

static int contains(const uint8_t *hay, size_t hn, const uint8_t *needle, size_t nn) {
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return 1;
    return 0;
}

/* ---- tests ---- */

static int32_t ALPHA = -1;

TEST(generated_ed25519_key_signs_and_its_public_blob_verifies) {
    ALPHA = psys_api_key_generate(NO_CTX, 1, "alpha", 5);
    CHECK(ALPHA >= 0);
    uint8_t blob[128];
    CHECK_EQ(psys_api_key_public(NO_CTX, ALPHA, blob, sizeof(blob)), 51);
    CHECK(memcmp(blob, "\0\0\0\x0b" "ssh-ed25519" "\0\0\0\x20", 19) == 0);
    uint8_t sig[64];
    CHECK_EQ(psys_api_key_sign(NO_CTX, ALPHA, "hello", 5, sig, 64), 64);
    CHECK_EQ(psys_api_ed25519_verify(NO_CTX, blob + 19, 32, "hello", 5, sig, 64), NEMA_OK);
    CHECK_EQ(psys_api_ed25519_verify(NO_CTX, blob + 19, 32, "hellO", 5, sig, 64), NEMA_ERR_AUTH);
    CHECK_EQ(psys_api_key_public(NO_CTX, ALPHA, blob, 50), NEMA_ERR_RANGE);
    CHECK_EQ(psys_api_key_sign(NO_CTX, ALPHA, "x", 1, sig, 63), NEMA_ERR_RANGE);
}

TEST(names_are_checked_and_not_reused) {
    CHECK_EQ(psys_api_key_generate(NO_CTX, 1, "alpha", 5), NEMA_ERR_EXIST);
    CHECK_EQ(psys_api_key_generate(NO_CTX, 1, "../x", 4), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_key_generate(NO_CTX, 1, "a.b", 3), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_key_generate(NO_CTX, 1, "", 0), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_key_generate(NO_CTX, 7, "gamma", 5), NEMA_ERR_UNSUPPORTED);
    CHECK_EQ(psys_api_key_load(NO_CTX, "nope", 4), NEMA_ERR_NOENT);
}

TEST(the_private_bytes_are_not_in_the_file) {
    char text[1024];
    size_t tl = make_openssh("none", text, sizeof(text));
    CHECK(tl > 0);
    int32_t h = psys_api_key_import(NO_CTX, text, (uint32_t)tl, "", 0, "imported", 8);
    CHECK(h >= 0);
    uint8_t seed[32], sk[64], file[512];
    unhex(SEED1, seed, 32);
    /* the 32-byte seed and the 64-byte secret must not appear in the stored record */
    size_t n = 0;
    char path[300];
    path_of(path, sizeof(path), "data/keystore/imported.key");
    CHECK(read_all(path, file, sizeof(file), &n));
    CHECK(!contains(file, n, seed, 32));
    uint8_t pk[32];
    unhex(PK1, pk, 32);
    prim_ed25519_keypair(sk, pk, seed);
    CHECK(!contains(file, n, sk, 64));
    CHECK(!contains(file, n, sk + 32, 32)); /* the record is encrypted as a whole, public half included */
}

TEST(imported_rfc8032_key_gives_the_known_signature) {
    uint8_t blob[128], sig[64];
    int32_t h = psys_api_key_load(NO_CTX, "imported", 8);
    CHECK(h >= 0);
    CHECK_EQ(psys_api_key_public(NO_CTX, h, blob, sizeof(blob)), 51);
    CHECK(eq_hex(blob + 19, 32, PK1));
    CHECK_EQ(psys_api_key_sign(NO_CTX, h, "", 0, sig, 64), 64);
    CHECK(eq_hex(sig, 64, SIG1));
}

TEST(encrypted_and_other_openssh_keys_are_refused_with_unsupported) {
    char text[1024];
    size_t tl = make_openssh("aes256-ctr", text, sizeof(text));
    CHECK_EQ(psys_api_key_import(NO_CTX, text, (uint32_t)tl, "", 0, "encrypted", 9), NEMA_ERR_UNSUPPORTED);
    CHECK_EQ(psys_api_key_import(NO_CTX, "not a key", 9, "", 0, "junk", 4), NEMA_ERR_INVAL);
}

TEST(list_names_the_stored_keys_and_delete_removes_one) {
    char list[256];
    int n = psys_api_key_list(NO_CTX, list, sizeof(list));
    CHECK(n > 0);
    list[n] = 0;
    CHECK(strstr(list, "alpha\n") != NULL);
    CHECK(strstr(list, "imported\n") != NULL);
    CHECK(strstr(list, ".dk") == NULL && strstr(list, ".tmp") == NULL);
    CHECK_EQ(psys_api_key_list(NO_CTX, list, 4), NEMA_ERR_RANGE);
    CHECK_EQ(psys_api_key_delete(NO_CTX, "alpha", 5), NEMA_OK);
    CHECK_EQ(psys_api_key_delete(NO_CTX, "alpha", 5), NEMA_ERR_NOENT);
    CHECK_EQ(psys_api_key_load(NO_CTX, "alpha", 5), NEMA_ERR_NOENT);
    n = psys_api_key_list(NO_CTX, list, sizeof(list));
    list[n] = 0;
    CHECK(strstr(list, "alpha") == NULL);
}

TEST(a_key_file_cannot_be_moved_under_another_name) {
    char from[300], to[300];
    path_of(from, sizeof(from), "data/keystore/imported.key");
    path_of(to, sizeof(to), "data/keystore/copied.key");
    uint8_t buf[512];
    size_t n = 0;
    CHECK(read_all(from, buf, sizeof(buf), &n));
    CHECK(write_all(to, buf, n));
    CHECK_EQ(psys_api_key_load(NO_CTX, "copied", 6), NEMA_ERR_AUTH);
    remove(to);
}

TEST(a_tampered_key_file_is_refused) {
    char path[300];
    path_of(path, sizeof(path), "data/keystore/imported.key");
    uint8_t buf[512];
    size_t n = 0;
    CHECK(read_all(path, buf, sizeof(buf), &n));
    buf[40] ^= 0x01;
    CHECK(write_all(path, buf, n));
    CHECK_EQ(psys_api_key_load(NO_CTX, "imported", 8), NEMA_ERR_AUTH);
    buf[40] ^= 0x01;
    CHECK(write_all(path, buf, n));
    CHECK(psys_api_key_load(NO_CTX, "imported", 8) >= 0);
}

TEST(device_key_is_stable_and_its_secret_is_stored_apart) {
    uint8_t a[32], b[32], secret[64];
    CHECK_EQ(psys_keystore_device_key(a), NEMA_OK);
    CHECK_EQ(psys_keystore_device_key(b), NEMA_OK);
    CHECK(memcmp(a, b, 32) == 0);
    char path[300];
    path_of(path, sizeof(path), "data/keystore/.dk");
    size_t n = 0;
    CHECK(read_all(path, secret, sizeof(secret), &n));
    CHECK_EQ(n, 32);
    CHECK(!contains(a, 32, secret, 32));
}

TEST(a_full_table_refuses_more_loads) {
    /* the table holds 8 keys per process; the native table is shared by these tests, so count to the limit */
    int32_t r = 0;
    for (int i = 0; i < 10; i++) {
        r = psys_api_key_load(NO_CTX, "imported", 8);
        if (r < 0) break;
    }
    CHECK_EQ(r, NEMA_ERR_FULL);
}

static void setup(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(ROOT, sizeof(ROOT), "%s/palanu-keys-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(ROOT)) abort();
    void *fs = nema_posixfs_create(ROOT);
    if (!fs) abort();
    nema_vfs_mount("/", nema_posixfs_ops(), fs);
    nema_fs_mkdir("/data");
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    setup();
    RUN(generated_ed25519_key_signs_and_its_public_blob_verifies);
    RUN(names_are_checked_and_not_reused);
    RUN(the_private_bytes_are_not_in_the_file);
    RUN(imported_rfc8032_key_gives_the_known_signature);
    RUN(encrypted_and_other_openssh_keys_are_refused_with_unsupported);
    RUN(list_names_the_stored_keys_and_delete_removes_one);
    RUN(a_key_file_cannot_be_moved_under_another_name);
    RUN(a_tampered_key_file_is_refused);
    RUN(device_key_is_stable_and_its_secret_is_stored_apart);
    RUN(a_full_table_refuses_more_loads);
    return TEST_DONE();
}
