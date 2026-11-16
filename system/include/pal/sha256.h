/* SHA-256 (FIPS 180-4). Small, portable, and the same on the board and the host, so a .papp hash can be checked
 * anywhere without pulling in a crypto library for one function. */
#ifndef PAL_SHA256_H
#define PAL_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} pal_sha256_t;

void pal_sha256_init(pal_sha256_t *c);
void pal_sha256_update(pal_sha256_t *c, const void *data, size_t n);
void pal_sha256_final(pal_sha256_t *c, uint8_t out[32]);

#endif
