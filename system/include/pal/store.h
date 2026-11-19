/* PalanuOS app catalogue (ADR-0015): a list of packages at a URL (the first line of /etc/store.url; the project's catalogue on
 * GitHub when there is none), fetched over HTTPS, and packages
 * downloaded, checked against the SHA-256 the list gives, and installed as `app install` does (the trust policy still applies:
 * a package needs a trusted signature, or developer mode). Blocking: call it from a process or a task that may wait.
 *
 * The list is text: a first line "palanu-store 1", then one package per line, fields separated by tabs:
 *   id  version  name  size  sha256(hex)  path  summary
 * path is relative to the list's URL (or a full URL). forge/tools/store.ts writes it from a folder of packages. */
#ifndef PAL_STORE_H
#define PAL_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"

#define PAL_STORE_MAX 48

typedef struct {
    char id[24], version[16], name[32];
    uint32_t size;
    uint8_t sha256[32];
    char path[96];
    char summary[80];
} pal_store_entry_t;

/* Fetches the list from store.url into the catalogue (replacing it). NEMA_ERR_PROTO when it is not a store list. */
nema_err_t pal_store_refresh(char *msg, size_t msgcap);
size_t pal_store_count(void);
bool pal_store_get(size_t i, pal_store_entry_t *out);
bool pal_store_find(const char *id, pal_store_entry_t *out);
/* The version installed now ("" when the app is not installed). */
void pal_store_installed(const char *id, char *version, size_t cap);
/* Downloads, checks and installs the package id from the catalogue. progress (may be NULL) gets the bytes so far. msg gets the
 * reason when it fails. */
nema_err_t pal_store_install(const char *id, void (*progress)(uint32_t done, uint32_t total, void *ctx), void *ctx, char *msg,
                             size_t msgcap);
/* Parses a list (for the tests and pal_store_refresh): returns the entries read, or -1 when it is not a store list. */
int pal_store_parse(const char *text, size_t len, pal_store_entry_t *out, size_t max);

#endif
