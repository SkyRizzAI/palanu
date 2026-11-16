/* Digests and HMAC in one state (system/src/crypto_mac.c), built on the cryptoimpl.h hash primitives. A hash context
 * uses only `in`; an HMAC context also uses `out`, which already absorbed the outer key pad at init. */
#ifndef SYSTEM_CRYPTO_MAC_H
#define SYSTEM_CRYPTO_MAC_H

#include <stddef.h>
#include <stdint.h>

#include "cryptoimpl.h"

typedef struct {
    ci_hash_t in;
    ci_hash_t out; /* HMAC only */
    uint8_t alg;   /* 0 when unused */
    uint8_t hmac;  /* 1 for HMAC */
} crypto_mac_t;

size_t crypto_mac_len(int alg); /* digest bytes, 0 for an unknown algorithm */
int crypto_mac_hash(crypto_mac_t *m, int alg);
int crypto_mac_hmac(crypto_mac_t *m, int alg, const uint8_t *key, size_t klen);
void crypto_mac_update(crypto_mac_t *m, const void *d, size_t n);
/* Writes crypto_mac_len() bytes to out (the caller checks the capacity first) and wipes the state. */
size_t crypto_mac_final(crypto_mac_t *m, uint8_t *out);

#endif
