/* Keystore: private keys are made, imported and used inside the system. An app gets a handle and the public
 * key, and asks for signatures; the private bytes never enter the app's memory. Keys are stored encrypted under a device
 * key in the system's data area (/data/keystore), which no app can open through the file API. Needs the "crypto.keys"
 * permission in the manifest. The device key comes from a random secret made at first use: on the ESP that random source is
 * only true random while the radio runs (Wi-Fi or BLE), so make the first key after the radio has started. */
#ifndef PALANU_API_KEYS_H
#define PALANU_API_KEYS_H

#include "annot.h"

/* Key types for pal_key_generate. */
#define PAL_KEY_ED25519 1
#define PAL_KEY_ECDSA_P256 2

PAL_SINCE(1) PAL_CAP("crypto.keys") PAL_WASM(key_generate)
PAL_DOC("Makes a new key of type (PAL_KEY_*) under name, stores it, and returns a handle. Names are [A-Za-z0-9_-], 1 to 32.")
int32_t pal_key_generate(int32_t type, PAL_IN(nlen) const char *name, uint32_t nlen);

PAL_SINCE(1) PAL_CAP("crypto.keys") PAL_WASM(key_load)
PAL_DOC("Loads the stored key name into this app's key table and returns a handle.")
int32_t pal_key_load(PAL_IN(nlen) const char *name, uint32_t nlen);

PAL_SINCE(1) PAL_CAP("crypto.keys") PAL_WASM(key_import)
PAL_DOC("Imports an unencrypted OpenSSH ed25519 key (-----BEGIN OPENSSH PRIVATE KEY-----) under name. Other forms: PAL_ERR_UNSUPPORTED.")
int32_t pal_key_import(PAL_IN(blen) const void *blob, uint32_t blen, PAL_IN(plen) const char *passphrase, uint32_t plen,
                       PAL_IN(nlen) const char *name, uint32_t nlen);

PAL_SINCE(1) PAL_CAP("crypto.keys") PAL_WASM(key_public)
PAL_DOC("Writes the public key as an SSH wire blob (ssh-ed25519 plus 32 bytes, or the ecdsa-sha2-nistp256 form). Returns its length.")
int32_t pal_key_public(int32_t h, PAL_OUT(cap) void *out, uint32_t cap);

PAL_SINCE(1) PAL_CAP("crypto.keys") PAL_WASM(key_sign)
PAL_DOC("Signs data with the key: ed25519 gives 64 bytes; ecdsa signs its SHA-256 digest and gives r || s (64 bytes). Returns the length.")
int32_t pal_key_sign(int32_t h, PAL_IN(len) const void *data, uint32_t len, PAL_OUT(cap) void *sig, uint32_t cap);

PAL_SINCE(1) PAL_CAP("crypto.keys") PAL_WASM(key_list)
PAL_DOC("Writes the names of the stored keys, one per line, into out. Returns the length.")
int32_t pal_key_list(PAL_OUT(cap) void *out, uint32_t cap);

PAL_SINCE(1) PAL_CAP("crypto.keys") PAL_WASM(key_delete)
PAL_DOC("Deletes the stored key name. Handles already loaded from it keep working until the app ends.")
int32_t pal_key_delete(PAL_IN(nlen) const char *name, uint32_t nlen);

#endif
