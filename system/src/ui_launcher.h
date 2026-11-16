/* The launcher skins (ui_launcher.c): the four the reference skins, strip, grid, compact and list, drawn by kui_launcher.c. launcher.c
 * owns the view and the keys; a skin draws its area and reports nothing back. Adding a skin: a kui_skin_* in kui_launcher.c,
 * a name in pal_ui_launcher_name (ui_prefs.c), a case in ui_skin_draw and in the choices of settings_api.c and settings_display.c. */
#ifndef SYSTEM_UI_LAUNCHER_H
#define SYSTEM_UI_LAUNCHER_H

#include "pal/icons_data.h"
#include "pal/kui.h"
#include "pal/registry.h"

#define UI_LAUNCHER_MAX 32 /* launchable entries a skin sees; the rest are listed by the registry but not drawn */

typedef struct {
    const pal_reg_app_t *v; /* the entries, in launcher order */
    int n;
    int sel;
    const char *title; /* the device name (strip's title) */
} ui_entries_t;

/* Draws the entries in the skin of style (PAL_UI_LAUNCHER_*) into the clip of k (the area under the status bar). */
void ui_skin_draw(kui_canvas_t *k, int style, const ui_entries_t *e);

/* The icon name of an entry: its manifest icon, else its id. */
const char *ui_entry_icon(const pal_reg_app_t *e);

/* The reference entries (launcher_fixture.c): icon names starting with "ref." resolve to them. */
const pal_icon_t *ui_fixture_icon(const char *name);
/* Fills out with the seven the reference entries; returns how many. */
int ui_fixture_entries(pal_reg_app_t *out, int max);
/* The reference app list (applist.c, uishow applist ref): the one entry Terminal. */
int ui_fixture_apps(pal_reg_app_t *out, int max);

#endif
