/* Runtime permissions (ADR-0014): an installed app that declares a sensitive permission is asked about it the first time it uses
 * it (Allow or Don't allow), and the answer is kept per app in /data/perms. A permission the user denied is not asked again; the
 * call fails with PAL_ERR_PERM, as if the manifest did not have it. Settings > Apps > the app shows each one and can change it or
 * reset them all. Built-in apps are not asked: they are part of the system. */
#ifndef PAL_PERMS_H
#define PAL_PERMS_H

#include <stdbool.h>
#include <stdint.h>

#include "nema/types.h"

/* The manifest permissions that are asked at first use (pal/papp.h bits). The others are granted with the manifest. */
#define PAL_PERM_ASKED 0x0000017Eu /* net, fs.home, fs.media, net.listen, proc.spawn, crypto.keys, settings.write */

typedef enum { PAL_PERM_ASK = 0, PAL_PERM_ALLOWED, PAL_PERM_DENIED } pal_perm_state_t;

pal_perm_state_t psys_perm_state(const char *app_id, uint32_t bit);
nema_err_t psys_perm_set(const char *app_id, uint32_t bit, pal_perm_state_t st); /* ASK forgets the answer */
nema_err_t psys_perm_reset(const char *app_id);                                  /* every permission back to Ask */

/* For the API runtime: may the app use bit now? Asks the user (the app waits) when it was not decided. Where nobody can be asked
 * (no display, no desktop yet, a host test) the manifest decides, as before ADR-0014, and nothing is kept. */
bool psys_perm_check(const char *app_id, const char *app_name, uint32_t bit);
/* A test replaces the question: return true for Allow. NULL restores the dialog. */
void psys_perm_set_asker(bool (*ask)(const char *app_name, uint32_t bit));
const char *psys_perm_what(uint32_t bit); /* "use the network" ... for the question and Settings */

#endif
