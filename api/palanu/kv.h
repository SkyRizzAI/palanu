/* Per-app key-value store (API level 2). Each app keeps its own keys in /apps/<id>/kv.dat, outside its /data namespace, so
 * the file API cannot reach them and no other app can read them. No capability is needed. Keys are 1 to 32 characters of
 * [a-z0-9._-]; a value is up to 1024 bytes; an app holds at most 64 keys. Writes are durable: a power cut leaves the old
 * value or the new one, and a half-written record is ignored. Native code and the built-in apps use the id "system". */
#ifndef PALANU_API_KV_H
#define PALANU_API_KV_H

#include "annot.h"

/* Limits of the store (the values the errors below refer to). */
#define PAL_KV_KEY_MAX 32
#define PAL_KV_VALUE_MAX 1024
#define PAL_KV_KEYS_MAX 64

PAL_SINCE(2) PAL_WASM(kv_get)
PAL_DOC("Copies the value of key into buf, as many bytes as it has. Returns its length, PAL_ERR_NOENT for an unknown key, PAL_ERR_RANGE when cap is smaller than the value, or PAL_ERR_INVAL for a bad key.")
int32_t pal_kv_get(const char *key, PAL_OUT(cap) void *buf, uint32_t cap);

PAL_SINCE(2) PAL_WASM(kv_set)
PAL_DOC("Stores len bytes of val under key, replacing an old value. Returns 0, PAL_ERR_INVAL (bad key, or len over 1024), or PAL_ERR_NOSPC when the app already holds 64 keys.")
int32_t pal_kv_set(const char *key, PAL_IN(len) const void *val, uint32_t len);

PAL_SINCE(2) PAL_WASM(kv_del)
PAL_DOC("Deletes key. Returns 0, or PAL_ERR_NOENT when the key is not stored.")
int32_t pal_kv_del(const char *key);

PAL_SINCE(2) PAL_WASM(kv_keys)
PAL_DOC("Writes the stored keys into out, each followed by a newline. Returns the length, or PAL_ERR_RANGE when cap is too small.")
int32_t pal_kv_keys(PAL_OUT(cap) void *out, uint32_t cap);

#endif
