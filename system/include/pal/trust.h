/* Trust for installed packages (system/src/trust.c, docs/PAPP.md). The verifier and the policy are pure apart from reading
 * /etc/trust and /data/settings. */
#ifndef PAL_TRUST_H
#define PAL_TRUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal/papp.h"

/* Public keys from /etc/trust (raw 32 bytes or hex) and the built-in release keys. Returns how many were stored. */
size_t psys_trust_load(uint8_t (*keys)[32], size_t max);
/* The signature over a package hash: PAL_SIG_NONE when unsigned, TRUSTED when a listed key verifies it. */
pal_sig_t psys_trust_verify(const uint8_t hash[32], const uint8_t *sig, const uint8_t (*keys)[32], size_t nkeys);
/* dev.mode=1 in /data/settings. */
/* The factory default of developer mode: ON in a development build, so `palanu deploy` works on a fresh board; OFF in a release
 * build (`PAL_RELEASE=1 ./pal build`, which `./pal release` does), where only packages signed by a key in /etc/trust, or the
 * built-in apps, run until the owner turns it on in Settings > Developer. An explicit dev.mode=0 or =1 always wins. */
#ifdef PAL_RELEASE_BUILD
#define PSYS_DEV_MODE_DEFAULT 0
#else
#define PSYS_DEV_MODE_DEFAULT 1
#endif

bool psys_dev_mode(void);
/* Accept or refuse a package by its signature and the permissions it asks for. note gets the reason or the warning. */
nema_err_t psys_policy(pal_sig_t sig, uint32_t caps, bool dev, char *note, size_t cap);

#endif
