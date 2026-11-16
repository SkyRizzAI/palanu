/* Internal to the system: the blocking waits of the dialogs. A caller on another task opens the dialog (the desktop draws
 * it), then sleeps until its callback fills the waiter. Shared by dialog.c and textinput.c. */
#ifndef SYSTEM_DIALOG_INT_H
#define SYSTEM_DIALOG_INT_H

#include <stdbool.h>

#include "nema/obj.h"
#include "pal/dialog.h"

typedef struct {
    nema_obj_t obj;
    volatile bool done;
    bool ok;
    char text[PAL_DIALOG_TEXT];
} dlg_waiter_t;

/* Prepares w. False (and an error in the log) when the caller is the desktop task, which must not wait, or when the desktop
 * is not running. */
bool dlg_wait_begin(dlg_waiter_t *w, const char *who);
/* Sleeps until w->done. The waiter lives on the caller's stack: it must not return before the callback has run. */
void dlg_wait_end(dlg_waiter_t *w);

#endif
