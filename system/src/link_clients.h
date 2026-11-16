/* The paired clients of Palanu Link (docs/PLP.md, "Pairing"): one encrypted record per client in /data/link/clients,
 * the device identifier, and the link settings file (/data/link/config). Records are ChaCha20-Poly1305 under the device
 * key of the keystore (psys_keystore_device_key), so a copied record is useless on another device. */
#ifndef SYSTEM_LINK_CLIENTS_H
#define SYSTEM_LINK_CLIENTS_H

#include <stddef.h>
#include <stdint.h>

#define LC_NAME_MAX 24
#define LC_MAX 8 /* paired clients at once */

typedef struct {
    uint8_t id[8];
    char name[LC_NAME_MAX + 1];
    uint8_t token[32];
    uint32_t created; /* epoch seconds */
    uint32_t last_used;
} lc_rec_t;

/* Hex helpers: enc writes 2n+1 bytes; dec reads exactly 2n hex digits. */
void lc_hex_enc(const uint8_t *b, size_t n, char *out);
int lc_hex_dec(const char *s, uint8_t *out, size_t n); /* 0 when the text is not exactly 2n hex digits */

int lc_device_id(char out[17]);                       /* creates the identifier on first use */
int lc_count(void);                                   /* paired clients, or a negative error */
int lc_load(const uint8_t id[8], lc_rec_t *r);        /* NEMA_ERR_NOENT when unknown; NEMA_ERR_AUTH when altered */
int lc_store(const lc_rec_t *r);                      /* creates or replaces the record */
int lc_remove(const uint8_t id[8]);                   /* NEMA_ERR_NOENT when unknown */
int lc_ids(uint8_t ids[][8], int cap);                /* the ids in the store, up to cap; returns how many */
size_t lc_list_text(char *out, size_t cap);           /* one line per client */

int lc_config_get(const char *key, char *out, size_t cap); /* NEMA_ERR_NOENT when unset */
int lc_config_set(const char *key, const char *value);

#endif
