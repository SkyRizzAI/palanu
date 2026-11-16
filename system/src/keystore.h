/* The keystore's storage: one encrypted file per key in /data/keystore, where no app can reach (the fs namespace
 * shows only /home and /tmp). Each file is ChaCha20-Poly1305 under the device key, with its type and name in the
 * associated data, so a file cannot be renamed into another key's place. Private bytes leave this file only as the
 * plaintext the key API (system/api/keys.c) copies into its table. */
#ifndef SYSTEM_KEYSTORE_H
#define SYSTEM_KEYSTORE_H

#include <stddef.h>
#include <stdint.h>

#define KS_DIR "/data/keystore"
#define KS_NAME_MAX 32
#define KS_PLAIN_LEN 129 /* the key material: secret[64] then pub[65] (layout of ct_key_t) */

/* The device key, the one place it comes from: HMAC-SHA256 over a label, keyed with the per-device random secret in
 * KS_DIR/.dk (created at first use). Moving that secret into the eFuse HMAC key (SOC_HMAC_SUPPORTED) is the later
 * hardening step; the derivation stays the same. Returns NEMA_OK, or NEMA_ERR_IO when keys exist but the secret is
 * gone (they could never be opened again, so nothing is regenerated). */
int psys_keystore_device_key(uint8_t out[32]);

/* A name is 1 to 32 of [A-Za-z0-9_-]: no dot, so the .dk secret and .tmp files can never be a key. */
int ks_name_ok(const char *name, size_t n);
int ks_exists(const char *name);
int ks_save(const char *name, uint8_t type, const uint8_t plain[KS_PLAIN_LEN]);
int ks_load(const char *name, uint8_t *type, uint8_t plain[KS_PLAIN_LEN]);
int ks_delete(const char *name);
/* The names, one per line, into out. Returns the length, or NEMA_ERR_RANGE when they do not fit. */
int ks_list(char *out, size_t cap);

/* An OpenSSH private key file (keyfmt.c) into the layout above. Unencrypted ssh-ed25519 only: other forms give
 * NEMA_ERR_UNSUPPORTED. */
int kf_import_openssh(const uint8_t *text, size_t len, uint8_t *type, uint8_t plain[KS_PLAIN_LEN]);

#endif
