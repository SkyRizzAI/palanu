/* The kv module of the palanu API through the psys_api_kv_* functions, with a native caller (no context: the id is
 * "system"). The store is a host directory mounted at "/", so a restart is a real one: the mount is taken down and put
 * back, and the next call reads the file alone. Torn and corrupt records are made by cutting and editing kv.dat itself.
 * Per-app isolation is checked by the wasm-level test (test_api_svc.c), which runs apps with their own ids. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../system/src/api_ctx.h"
#include "../../system/src/api_impl.h"
#include "nema/nema.h"
#include "test.h"

static char ROOT[256];
static api_ctx_t *NO_CTX = NULL;
static const char KV_FILE[] = "apps/system/kv.dat";

/* Length of a record: "KV" op klen vlen(2) key value crc(4). */
static uint32_t rec_len(uint32_t klen, uint32_t vlen) { return 6 + klen + vlen + 4; }

static void path_of(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s/%s", ROOT, rel); }

static long file_size(const char *rel) {
    char p[400];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static size_t read_file(const char *rel, uint8_t *buf, size_t cap) {
    char p[400];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static int write_file(const char *rel, const uint8_t *buf, size_t n) {
    char p[400];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n;
}

/* A restart: the mount goes away and comes back on the same host directory. */
static void restart(void) {
    nema_vfs_unmount("/");
    void *fs = nema_posixfs_create(ROOT);
    if (!fs) abort();
    nema_vfs_mount("/", nema_posixfs_ops(), fs);
}

static int32_t kv_set(const char *k, const char *v) { return psys_api_kv_set(NO_CTX, k, v, (uint32_t)strlen(v)); }

static int32_t kv_get_str(const char *k, char *out, uint32_t cap) {
    int32_t n = psys_api_kv_get(NO_CTX, k, out, cap);
    if (n >= 0 && (uint32_t)n < cap) out[n] = 0;
    return n;
}

/* Deletes every key the store holds, so a test starts from an empty store. */
static void clear_all(void) {
    static char keys[64 * 34 + 1]; /* 64 keys of 32 characters and a newline, and the NUL */
    int32_t n = psys_api_kv_keys(NO_CTX, keys, sizeof(keys));
    if (n <= 0) return;
    keys[n] = 0;
    char *p = keys;
    while (*p) {
        char *nl = strchr(p, '\n');
        *nl = 0;
        psys_api_kv_del(NO_CTX, p);
        p = nl + 1;
    }
}

/* ---- tests ---- */

TEST(set_get_delete_and_keys) {
    clear_all();
    CHECK_EQ(kv_set("alpha", "one"), NEMA_OK);
    CHECK_EQ(kv_set("beta", "two"), NEMA_OK);
    char buf[64];
    CHECK_EQ(kv_get_str("alpha", buf, sizeof(buf)), 3);
    CHECK(strcmp(buf, "one") == 0);
    CHECK_EQ(kv_set("alpha", "uno"), NEMA_OK); /* replaces */
    CHECK_EQ(kv_get_str("alpha", buf, sizeof(buf)), 3);
    CHECK(strcmp(buf, "uno") == 0);
    int32_t kn = psys_api_kv_keys(NO_CTX, buf, sizeof(buf));
    CHECK_EQ(kn, 11); /* "alpha\nbeta\n" */
    buf[kn > 0 ? kn : 0] = 0;
    CHECK(strcmp(buf, "alpha\nbeta\n") == 0);
    CHECK_EQ(psys_api_kv_keys(NO_CTX, buf, 5), NEMA_ERR_RANGE);
    CHECK_EQ(psys_api_kv_del(NO_CTX, "alpha"), NEMA_OK);
    CHECK_EQ(psys_api_kv_del(NO_CTX, "alpha"), NEMA_ERR_NOENT);
    CHECK_EQ(kv_get_str("alpha", buf, sizeof(buf)), NEMA_ERR_NOENT);
    CHECK_EQ(psys_api_kv_keys(NO_CTX, buf, sizeof(buf)), 5); /* "beta\n" */
}

TEST(empty_value_and_small_buffer) {
    CHECK_EQ(psys_api_kv_set(NO_CTX, "empty", NULL, 0), NEMA_OK);
    char buf[8];
    CHECK_EQ(psys_api_kv_get(NO_CTX, "empty", buf, 0), 0);
    CHECK_EQ(kv_set("three", "abc"), NEMA_OK);
    CHECK_EQ(psys_api_kv_get(NO_CTX, "three", buf, 2), NEMA_ERR_RANGE);
    CHECK_EQ(psys_api_kv_get(NO_CTX, "three", buf, 3), 3);
}

TEST(key_names_are_checked) {
    char k33[34], k32[33];
    memset(k33, 'a', 33);
    k33[33] = 0;
    memset(k32, 'b', 32);
    k32[32] = 0;
    CHECK_EQ(kv_set("", "x"), NEMA_ERR_INVAL);
    CHECK_EQ(kv_set(k33, "x"), NEMA_ERR_INVAL);
    CHECK_EQ(kv_set(k32, "x"), NEMA_OK); /* 32 is the most */
    CHECK_EQ(kv_set("Upper", "x"), NEMA_ERR_INVAL);
    CHECK_EQ(kv_set("sp ace", "x"), NEMA_ERR_INVAL);
    CHECK_EQ(kv_set("sl/ash", "x"), NEMA_ERR_INVAL);
    CHECK_EQ(kv_set("ok.name_1-x", "x"), NEMA_OK);
    CHECK_EQ(psys_api_kv_set(NO_CTX, NULL, "x", 1), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_kv_del(NO_CTX, "Upper"), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_kv_get(NO_CTX, "Upper", NULL, 0), NEMA_ERR_INVAL);
}

TEST(value_size_is_limited_to_1024) {
    static uint8_t v[1025];
    memset(v, 7, sizeof(v));
    CHECK_EQ(psys_api_kv_set(NO_CTX, "big", v, 1024), NEMA_OK);
    CHECK_EQ(psys_api_kv_set(NO_CTX, "big", v, 1025), NEMA_ERR_INVAL);
    static uint8_t out[1024];
    CHECK_EQ(psys_api_kv_get(NO_CTX, "big", out, sizeof(out)), 1024);
    CHECK(out[0] == 7 && out[1023] == 7);
}

TEST(sixty_four_keys_and_no_more) {
    clear_all();
    char k[8];
    for (int i = 0; i < 64; i++) {
        snprintf(k, sizeof(k), "lim%02d", i);
        CHECK_EQ(kv_set(k, "v"), NEMA_OK);
    }
    CHECK_EQ(kv_set("lim64", "v"), NEMA_ERR_NOSPC);
    CHECK_EQ(kv_set("lim00", "again"), NEMA_OK); /* an existing key is not a new one */
    CHECK_EQ(psys_api_kv_del(NO_CTX, "lim00"), NEMA_OK);
    CHECK_EQ(kv_set("lim64", "v"), NEMA_OK); /* the freed slot is used */
    clear_all();
}

TEST(values_survive_a_restart) {
    clear_all();
    CHECK_EQ(kv_set("keep", "after restart"), NEMA_OK);
    CHECK_EQ(kv_set("gone", "x"), NEMA_OK);
    CHECK_EQ(psys_api_kv_del(NO_CTX, "gone"), NEMA_OK);
    restart();
    char buf[64];
    CHECK_EQ(kv_get_str("keep", buf, sizeof(buf)), 13);
    CHECK(strcmp(buf, "after restart") == 0);
    CHECK_EQ(kv_get_str("gone", buf, sizeof(buf)), NEMA_ERR_NOENT);
}

TEST(a_torn_last_record_is_ignored_and_the_next_write_repairs_the_log) {
    clear_all();
    CHECK_EQ(kv_set("t.one", "1111"), NEMA_OK);
    CHECK_EQ(kv_set("t.two", "2222222"), NEMA_OK);
    long full = file_size(KV_FILE);
    CHECK_EQ(full, (long)(rec_len(5, 4) + rec_len(5, 7)));
    /* cut the last record in the middle: the power went out while it was written */
    static uint8_t buf[4096];
    size_t n = read_file(KV_FILE, buf, sizeof(buf));
    CHECK_EQ((long)n, full);
    CHECK(write_file(KV_FILE, buf, n - 5));
    char v[32];
    CHECK_EQ(kv_get_str("t.two", v, sizeof(v)), NEMA_ERR_NOENT);
    CHECK_EQ(kv_get_str("t.one", v, sizeof(v)), 4);
    CHECK(strcmp(v, "1111") == 0);
    /* the next write rewrites the log without the torn tail, and keeps what was valid */
    CHECK_EQ(kv_set("t.three", "3"), NEMA_OK);
    CHECK_EQ(file_size(KV_FILE), (long)(rec_len(5, 4) + rec_len(7, 1)));
    CHECK_EQ(kv_get_str("t.one", v, sizeof(v)), 4);
    CHECK_EQ(kv_get_str("t.three", v, sizeof(v)), 1);
    restart();
    CHECK_EQ(kv_get_str("t.three", v, sizeof(v)), 1);
}

TEST(a_corrupt_record_ends_the_log_and_nothing_after_it_is_read) {
    clear_all();
    CHECK_EQ(kv_set("c.first", "AAAA"), NEMA_OK);
    CHECK_EQ(kv_set("c.second", "BBBB"), NEMA_OK);
    static uint8_t buf[4096];
    size_t n = read_file(KV_FILE, buf, sizeof(buf));
    CHECK_EQ((long)n, (long)(rec_len(7, 4) + rec_len(8, 4)));
    /* one byte of the value of c.first changes: its CRC fails, so the log ends before it */
    size_t off = 6 + 7;
    buf[off] ^= 0x20;
    CHECK(write_file(KV_FILE, buf, n));
    char v[16];
    CHECK_EQ(kv_get_str("c.first", v, sizeof(v)), NEMA_ERR_NOENT);
    CHECK_EQ(kv_get_str("c.second", v, sizeof(v)), NEMA_ERR_NOENT);
    CHECK_EQ(kv_set("c.third", "C"), NEMA_OK); /* the log is rewritten from what is valid */
    CHECK_EQ(kv_get_str("c.third", v, sizeof(v)), 1);
    CHECK_EQ(file_size(KV_FILE), (long)rec_len(7, 1)); /* only c.third is live */
}

TEST(compaction_keeps_the_file_near_its_live_size) {
    clear_all();
    static uint8_t val[100];
    for (int i = 0; i < 10; i++) {
        memset(val, 'a' + i, sizeof(val));
        CHECK_EQ(psys_api_kv_set(NO_CTX, "churn", val, sizeof(val)), NEMA_OK);
    }
    /* ten records were written, but no more than half of the log is dead after any write: at most two records remain */
    long size = file_size(KV_FILE);
    CHECK(size <= (long)(2 * rec_len(5, sizeof(val))));
    static uint8_t out[100];
    CHECK_EQ(psys_api_kv_get(NO_CTX, "churn", out, sizeof(out)), 100);
    CHECK(out[0] == 'a' + 9 && out[99] == 'a' + 9);
}

TEST(deleting_every_key_leaves_an_empty_log) {
    clear_all();
    CHECK_EQ(psys_api_kv_keys(NO_CTX, (char[4]){0}, 0), 0);
    CHECK_EQ(file_size(KV_FILE), 0L);
    CHECK_EQ(psys_api_kv_del(NO_CTX, "churn"), NEMA_ERR_NOENT);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    const char *tmp = getenv("TMPDIR");
    snprintf(ROOT, sizeof(ROOT), "%s/palanu-kv-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(ROOT)) abort();
    nema_vfs_mount("/", nema_posixfs_ops(), nema_posixfs_create(ROOT));
    RUN(set_get_delete_and_keys);
    RUN(empty_value_and_small_buffer);
    RUN(key_names_are_checked);
    RUN(value_size_is_limited_to_1024);
    RUN(sixty_four_keys_and_no_more);
    RUN(values_survive_a_restart);
    RUN(a_torn_last_record_is_ignored_and_the_next_write_repairs_the_log);
    RUN(a_corrupt_record_ends_the_log_and_nothing_after_it_is_read);
    RUN(compaction_keeps_the_file_near_its_live_size);
    RUN(deleting_every_key_leaves_an_empty_log);
    return TEST_DONE();
}
