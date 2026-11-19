/* Firmware from the catalogue (pal/store.h, ADR-0016), end to end against a local HTTP server and a pretend update slot: a list
 * signed by a trusted key lets the image in, and only an image with the list's size and SHA-256 becomes the next boot; an unsigned
 * list needs developer mode; the firmware line stays out of the app list. Versions compare part by part as numbers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../system/src/crypto_prim.h"
#include "nema/nema.h"
#include "nema/net.h"
#include "nema/vfs.h"
#include "pal/board.h"
#include "pal/ota.h"
#include "pal/sha256.h"
#include "pal/store.h"
#include "test.h"

/* ---- a server that answers each request by its path ---- */
static int g_port;
static nema_obj_t *g_lst;
static char g_list[1024];
static uint8_t g_sig[64];
static bool g_have_sig;
static uint8_t g_image[3000];
static size_t g_image_len;

static void sha(const void *b, size_t n, uint8_t out[32]) {
    pal_sha256_t c;
    pal_sha256_init(&c);
    pal_sha256_update(&c, b, n);
    pal_sha256_final(&c, out);
}

static void send_all(nema_obj_t *s, const void *b, size_t n) {
    char head[128];
    int h = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)n);
    nema_obj_write(s, head, (size_t)h, 3000);
    nema_obj_write(s, b, n, 3000);
}

static void serve(void *arg) {
    (void)arg;
    for (;;) {
        nema_obj_t *s = NULL;
        if (nema_net_accept(g_lst, 60000, &s) != NEMA_OK) break;
        char req[512];
        size_t n = 0;
        while (n + 1 < sizeof(req)) {
            int r = nema_obj_read(s, req + n, sizeof(req) - 1 - n, 3000);
            if (r <= 0) break;
            n += (size_t)r;
            req[n] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[n] = 0;
        if (strstr(req, "GET /index.txt.sig ")) {
            if (g_have_sig) send_all(s, g_sig, 64);
            else nema_obj_write(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 45, 3000);
        } else if (strstr(req, "GET /index.txt ")) {
            send_all(s, g_list, strlen(g_list));
        } else if (strstr(req, "GET /fw.bin ")) {
            send_all(s, g_image, g_image_len);
        } else {
            nema_obj_write(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 45, 3000);
        }
        nema_obj_unref(s);
    }
}

/* ---- the pretend slot ---- */
static struct {
    size_t begun, written;
    bool ended, aborted;
} SL;
static nema_err_t sl_begin(size_t size) {
    memset(&SL, 0, sizeof(SL));
    SL.begun = size;
    return NEMA_OK;
}
static nema_err_t sl_write(const void *d, size_t n) {
    (void)d;
    SL.written += n;
    return NEMA_OK;
}
static nema_err_t sl_end(void) {
    SL.ended = true;
    return NEMA_OK;
}
static void sl_abort(void) { SL.aborted = true; }
static const pal_ota_ops_t SLOT = {sl_begin, sl_write, sl_end, sl_abort, NULL, NULL};

/* ---- fixtures ---- */
static uint8_t g_sk[64], g_pk[32];

static void write_file(const char *path, const void *b, size_t n) {
    nema_obj_t *f;
    if (nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) return;
    nema_obj_write(f, b, n, -1);
    nema_obj_unref(f);
}

static void set_dev(bool on) { write_file("/data/settings", on ? "dev.mode=1\n" : "dev.mode=0\n", 11); }

/* The list with one app and the firmware for "testboard"; sha_of_image false writes a wrong checksum. sign: index.txt.sig. */
static void publish(bool sha_of_image, bool sign) {
    uint8_t sum[32];
    sha(g_image, g_image_len, sum);
    if (!sha_of_image) sum[0] ^= 1;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    snprintf(g_list, sizeof(g_list),
             "palanu-store 1\n"
             "hello\t1.0\tHello\t10\t%s\thello.papp\tAn app\n"
             "firmware:testboard\t9.9.9\tPalanuOS\t%u\t%s\tfw.bin\tThe system\n"
             "firmware:otherboard\t9.9.9\tPalanuOS\t%u\t%s\tother.bin\tAnother board\n",
             hex, (unsigned)g_image_len, hex, (unsigned)g_image_len, hex);
    uint8_t lsum[32];
    sha(g_list, strlen(g_list), lsum);
    prim_ed25519_sign(g_sig, g_sk, lsum, 32);
    g_have_sig = sign;
}

TEST(versions_compare_as_numbers) {
    CHECK_EQ(pal_store_version_cmp("0.10.0", "0.9.1"), 1);
    CHECK_EQ(pal_store_version_cmp("0.9.0", "0.9.0"), 0);
    CHECK_EQ(pal_store_version_cmp("1.2", "1.2.1"), -1);
    CHECK_EQ(pal_store_version_cmp("1.0.0-rc1", "1.0.0"), 1); /* a suffix sorts by its text */
}

TEST(a_signed_list_installs_the_image_and_keeps_it_out_of_the_apps) {
    set_dev(false);
    publish(true, true);
    char msg[96];
    CHECK(pal_store_refresh(msg, sizeof(msg)) == NEMA_OK);
    CHECK(pal_store_signed());
    CHECK_EQ((int)pal_store_count(), 1); /* hello; the firmware lines are not apps */
    pal_store_entry_t e;
    CHECK(pal_store_firmware(&e) && !strcmp(e.version, "9.9.9") && !strcmp(e.path, "fw.bin"));
    CHECK(pal_store_update_firmware(NULL, NULL, msg, sizeof(msg)) == NEMA_OK);
    CHECK(SL.begun == g_image_len && SL.written == g_image_len && SL.ended && !SL.aborted);
}

TEST(a_wrong_checksum_is_aborted) {
    set_dev(false);
    publish(false, true);
    char msg[96];
    CHECK(pal_store_refresh(msg, sizeof(msg)) == NEMA_OK);
    CHECK(pal_store_update_firmware(NULL, NULL, msg, sizeof(msg)) == NEMA_ERR_AUTH);
    CHECK(SL.aborted && !SL.ended);
    CHECK(strstr(msg, "checksum") != NULL);
}

TEST(an_unsigned_list_needs_developer_mode) {
    publish(true, false);
    char msg[96];
    set_dev(false);
    CHECK(pal_store_refresh(msg, sizeof(msg)) == NEMA_OK);
    CHECK(!pal_store_signed());
    memset(&SL, 0, sizeof(SL));
    CHECK(pal_store_update_firmware(NULL, NULL, msg, sizeof(msg)) == NEMA_ERR_PERM);
    CHECK_EQ((int)SL.begun, 0); /* the slot was never touched */
    set_dev(true);
    CHECK(pal_store_update_firmware(NULL, NULL, msg, sizeof(msg)) == NEMA_OK);
    CHECK(SL.ended);
}

TEST(a_signature_by_an_unknown_key_is_not_trusted) {
    set_dev(false);
    publish(true, true);
    g_sig[0] ^= 0x40; /* as if another key had signed it */
    char msg[96];
    CHECK(pal_store_refresh(msg, sizeof(msg)) == NEMA_OK);
    CHECK(!pal_store_signed());
}

TEST(without_update_support_it_says_so) {
    pal_ota_set(NULL);
    char msg[96];
    CHECK(pal_store_update_firmware(NULL, NULL, msg, sizeof(msg)) == NEMA_ERR_UNSUPPORTED);
    pal_ota_set(&SLOT);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_net_init();
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(512u * 1024u));
    nema_fs_mkdir("/etc");
    nema_fs_mkdir("/etc/trust");
    nema_fs_mkdir("/data");
    nema_fs_mkdir("/tmp");
    static pal_board_desc_t board = {.name = "testboard"};
    pal_board_set(&board);
    pal_ota_set(&SLOT);

    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
    prim_ed25519_keypair(g_sk, g_pk, seed);
    write_file("/etc/trust/test.key", g_pk, 32);
    g_image_len = sizeof(g_image);
    for (size_t i = 0; i < g_image_len; i++) g_image[i] = (uint8_t)(i * 31 + 5);

    /* a free port for this run */
    for (int t = 0; t < 20 && !g_lst; t++) {
        g_port = 30000 + (int)((nema_port_time_us() + (uint64_t)t * 7919u) % 30000u);
        if (nema_net_listen(g_port, &g_lst) != NEMA_OK) g_lst = NULL;
    }
    CHECK(g_lst != NULL);
    nema_task_desc_t d = {.name = "httpd", .fn = serve, .stack = 8192, .role = NEMA_TASK_SYSTEM};
    nema_task_create(&d);
    char url[64];
    int n = snprintf(url, sizeof(url), "http://127.0.0.1:%d/index.txt\n", g_port);
    write_file("/etc/store.url", url, (size_t)n);

    RUN(versions_compare_as_numbers);
    RUN(a_signed_list_installs_the_image_and_keeps_it_out_of_the_apps);
    RUN(a_wrong_checksum_is_aborted);
    RUN(an_unsigned_list_needs_developer_mode);
    RUN(a_signature_by_an_unknown_key_is_not_trusted);
    RUN(without_update_support_it_says_so);
    return TEST_DONE();
}
