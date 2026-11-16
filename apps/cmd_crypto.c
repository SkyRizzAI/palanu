/* crypto-bench: what the crypto calls cost on this device. Prints the time per operation for X25519, Ed25519
 * sign and verify, SHA-256 over 64 KB, and AEAD seal of 1.5 KB packets (AES-128-GCM and ChaCha20-Poly1305). It calls the
 * same psys_api functions the wasm imports call, with a NULL context (a built-in command), so the numbers are the
 * system's cost without the wasm boundary. The orchestrator runs it on the board; the results go to docs/SIZE-BUDGETS.md.
 * Time comes from psys_api_now_ms (milliseconds), so each figure is an average over enough repetitions to be meaningful. */
#include <stdint.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "libapp.h"

/* The system side: system/api/crypto_hash.c, crypto.c, and keys.c. Declared here, as the app layer may not see
 * system/src; the prototypes are the same as the generated implementation header (api_impl.h). */
struct api_ctx;
int64_t psys_api_now_ms(struct api_ctx *c);
int32_t psys_api_random(struct api_ctx *c, void *buf, uint32_t len);
int32_t psys_api_hash_open(struct api_ctx *c, int32_t alg);
int32_t psys_api_hash_update(struct api_ctx *c, int32_t h, const void *d, uint32_t n);
int32_t psys_api_hash_final(struct api_ctx *c, int32_t h, void *out, uint32_t cap);
int32_t psys_api_aead_open(struct api_ctx *c, int32_t alg, const void *key, uint32_t klen);
int32_t psys_api_aead_seal(struct api_ctx *c, int32_t h, const void *nonce, uint32_t nlen, const void *aad, uint32_t alen,
                           void *buf, uint32_t len, void *tag, uint32_t tlen);
int32_t psys_api_x25519(struct api_ctx *c, void *out, uint32_t olen, const void *scalar, uint32_t slen, const void *point,
                        uint32_t plen);
int32_t psys_api_ed25519_verify(struct api_ctx *c, const void *pk, uint32_t pklen, const void *msg, uint32_t mlen,
                                const void *sig, uint32_t slen);
int32_t psys_api_key_generate(struct api_ctx *c, int32_t type, const char *name, uint32_t nlen);
int32_t psys_api_key_public(struct api_ctx *c, int32_t h, void *out, uint32_t cap);
int32_t psys_api_key_sign(struct api_ctx *c, int32_t h, const void *data, uint32_t len, void *sig, uint32_t cap);
int32_t psys_api_key_delete(struct api_ctx *c, const char *name, uint32_t nlen);

#define X25519_N 50
#define ED_N 20
#define SHA_CHUNK 4096u
#define SHA_CHUNKS 16u /* 64 KB */
#define AEAD_N 100
#define PACKET 1536u /* 1.5 KB */

/* One line: total ms, and the mean per operation with three decimals (integer arithmetic only). */
static void line(const char *what, int n, int64_t ms, const char *unit) {
    uint64_t per_us = n > 0 ? (uint64_t)ms * 1000u / (uint64_t)n : 0; /* ms * 1000 / n = microseconds per op */
    pal_printf("%-34s %4d x  total %6lu ms  per %s %lu.%03lu ms\n", what, n, (unsigned long)ms, unit,
               (unsigned long)(per_us / 1000u), (unsigned long)(per_us % 1000u));
}

static int cmd_crypto_bench(int argc, char **argv) {
    (void)argc, (void)argv;
    uint8_t k[32], pt[32], out[32], nonce[12] = {0}, tag[16];
    static uint8_t packet[PACKET] NEMA_EXT_BSS;
    static uint8_t chunk[SHA_CHUNK] NEMA_EXT_BSS;
    psys_api_random(NULL, k, sizeof(k));
    psys_api_random(NULL, pt, sizeof(pt));
    memset(chunk, 0x5a, sizeof(chunk));
    memset(packet, 0xa5, sizeof(packet));

    int64_t t0 = psys_api_now_ms(NULL);
    for (int i = 0; i < X25519_N; i++) psys_api_x25519(NULL, out, 32, k, 32, pt, 32), memcpy(pt, out, 32);
    line("x25519 (scalar mult)", X25519_N, psys_api_now_ms(NULL) - t0, "op");

    /* Ed25519: a key in the keystore, as an app would have it. Sign and verify are timed apart. */
    static const char name[] = "bench-ed25519";
    uint8_t pub[64], sig[64];
    int32_t h = psys_api_key_generate(NULL, 1, name, sizeof(name) - 1);
    if (h < 0) {
        pal_printf("crypto-bench: cannot make a bench key (%ld)\n", (long)h);
        return 1;
    }
    int32_t plen = psys_api_key_public(NULL, h, pub, sizeof(pub));
    t0 = psys_api_now_ms(NULL);
    for (int i = 0; i < ED_N; i++) psys_api_key_sign(NULL, h, packet, 64, sig, sizeof(sig));
    line("ed25519 sign", ED_N, psys_api_now_ms(NULL) - t0, "op");
    t0 = psys_api_now_ms(NULL);
    for (int i = 0; i < ED_N; i++) psys_api_ed25519_verify(NULL, pub + plen - 32, 32, packet, 64, sig, 64);
    line("ed25519 verify", ED_N, psys_api_now_ms(NULL) - t0, "op");
    psys_api_key_delete(NULL, name, sizeof(name) - 1);

    t0 = psys_api_now_ms(NULL);
    for (int r = 0; r < 3; r++) {
        int32_t hh = psys_api_hash_open(NULL, 2);
        for (uint32_t i = 0; i < SHA_CHUNKS; i++) psys_api_hash_update(NULL, hh, chunk, SHA_CHUNK);
        psys_api_hash_final(NULL, hh, out, 32);
    }
    line("sha-256 of 64 KB", 3, psys_api_now_ms(NULL) - t0, "64KB");

    int32_t g = psys_api_aead_open(NULL, 1, k, 16);
    t0 = psys_api_now_ms(NULL);
    for (int i = 0; i < AEAD_N; i++) psys_api_aead_seal(NULL, g, nonce, 12, NULL, 0, packet, PACKET, tag, 16);
    line("aes-128-gcm seal, 1.5 KB", AEAD_N, psys_api_now_ms(NULL) - t0, "pkt");

    int32_t c = psys_api_aead_open(NULL, 3, k, 32);
    t0 = psys_api_now_ms(NULL);
    for (int i = 0; i < AEAD_N; i++) psys_api_aead_seal(NULL, c, nonce, 12, NULL, 0, packet, PACKET, tag, 16);
    line("chacha20-poly1305 seal, 1.5 KB", AEAD_N, psys_api_now_ms(NULL) - t0, "pkt");
    return 0;
}

static const nema_app_t CRYPTO_APPS[] = {
    {"crypto-bench", "time the crypto calls: x25519, ed25519, sha-256, AEAD (uses a keystore key, then deletes it)",
     cmd_crypto_bench, 0, 0},
};

void pal_apps_register_crypto(void);
void pal_apps_register_crypto(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(CRYPTO_APPS); i++) nema_app_register(&CRYPTO_APPS[i]);
}
