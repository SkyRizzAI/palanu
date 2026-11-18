/* PalanuOS desktops: the registry of desktop environments.
 *
 * A desktop environment is what the user sees at home: the home view, the way apps are listed and launched, and the keys that
 * reach the launcher, the control center and the notifications. The compositor underneath (pal/desktop.h: layers, overlays,
 * status bar, lock, power) is the same for every desktop; a desktop only decides which view is home and how the apps show.
 *
 * Desktops register themselves (the classic one in ui_wire.c; a board or a simulator adds more, see pal/athena.h). Exactly one is
 * active. `desktop use ID` in the shell switches at run time and the choice is kept in the `desktop.env` setting, so it is
 * applied again at boot (pal_desktops_boot). Under every desktop is the system console (pal/console.h, ADR-0010): a desktop
 * that fails to start, or `desktop stop`, leaves no session running and the console in front. No other desktop is started in
 * its place. */
#ifndef PAL_DESKTOPS_H
#define PAL_DESKTOPS_H

#include <stdbool.h>
#include <stddef.h>

#include "nema/types.h"

#define PAL_DESKTOP_ENV_MAX 8

typedef struct {
    const char *id;    /* [a-z0-9_-], what `desktop use` takes and what `desktop.env` stores */
    const char *title; /* for people */
    const char *about; /* one line */
    /* Make this desktop the home: register its views once, set the home view and the home keys. Returns false when it cannot start. */
    bool (*start)(void);
    /* Undo what start changed that the next desktop does not set itself (a desktop may leave it NULL). */
    void (*stop)(void);
} pal_desktop_env_t;

/* The structure must outlive the registry. */
nema_err_t pal_desktops_register(const pal_desktop_env_t *env);
size_t pal_desktops_count(void);
const pal_desktop_env_t *pal_desktops_get(size_t index);
const pal_desktop_env_t *pal_desktops_find(const char *id);
const char *pal_desktops_active(void); /* the id of the active desktop; "" before any started */

/* Stop the active desktop and start ID, then show its home. NEMA_ERR_NOENT for an unknown id (nothing changes); NEMA_ERR_IO when
 * it failed to start (no session runs, the console is in front). Saves the choice in `desktop.env` when it worked. */
nema_err_t pal_desktops_use(const char *id);

/* Stop the active desktop: no session runs and the console takes the screen. */
void pal_desktops_stop(void);

/* At boot, after the views are registered: start the desktop named by `desktop.env`, unless boot.target is console or this is
 * safe mode. An unknown name or a failed start leaves the console in front with the reason. */
void pal_desktops_boot(void);

#endif
