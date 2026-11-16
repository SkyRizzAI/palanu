/* The Files operations menu (files_menu.c): the reference file ops list, a full screen list under the entry's name. */
#ifndef PAL_APPS_FILES_MENU_H
#define PAL_APPS_FILES_MENU_H

#include "nema/types.h"

#define FILES_MENU_ITEMS 16 /* rows at most */

/* Called with the picked row, or -1 when the menu is closed with Esc (or taken away by the desktop). It runs after the menu is
 * gone, so it may open another view. */
typedef void (*files_menu_cb_t)(int idx, void *ctx);

/* Opens the menu over the front view. items are kept, not copied: they must stay valid until the callback. Returns NEMA_ERR_BUSY
 * while another menu is open. */
nema_err_t files_menu_open(const char *title, const char *const *items, int n, files_menu_cb_t cb, void *ctx);

#endif
