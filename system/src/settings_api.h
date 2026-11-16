/* The settings table (settings_api.c) as the settings API (system/api/settings.c) uses it. */
#ifndef SYSTEM_SETTINGS_API_H
#define SYSTEM_SETTINGS_API_H

#include <stddef.h>

/* Text of the value of key into out (NUL-terminated). Returns its length, or a negative error (NOENT: unknown key or not
 * available). */
int psys_settings_get(const char *key, char *out, size_t cap);

/* Checks value for key, applies it, saves it, and calls the change callback. Returns 0 or a negative error (NOENT,
 * PERM for a read-only key, INVAL for a value out of range or malformed). */
int psys_settings_set(const char *key, const char *value);

#endif
