/* PalanuOS dialogs, pickers, text input and busy overlay.
 *
 * The non-blocking calls open an overlay and return at once; the answer arrives in a callback that runs on the desktop
 * task. They may be called from any task (the desktop applies the open on its own task). The blocking calls (*_wait) are
 * for other tasks only: they wait for the answer and return it. Called from the desktop task they return false at once and
 * log an error, because waiting there would stop the desktop. Every dialog takes the physical keyboard: Enter takes the
 * focused button, Esc is cancel, Left/Right (Fn+A/D) move between buttons. The focus starts on Cancel, the left button, as in
 * The reference confirm dialogs, so Enter alone cancels. Up to 2 dialogs at once, one text input, one picker and one busy
 * overlay, in addition to the overlay stack.
 */
#ifndef PAL_DIALOG_H
#define PAL_DIALOG_H

#include <stdbool.h>
#include <stddef.h>

#include "nema/types.h"

#define PAL_DIALOG_TITLE 32 /* bytes, NUL included */
#define PAL_DIALOG_TEXT 128 /* bytes, NUL included */
#define PAL_DIALOG_LABEL 16 /* bytes, NUL included */
#define PAL_PICKER_ITEMS 12 /* the Files and Logs menus have more than eight entries; the list scrolls past the screen height */

typedef void (*pal_dialog_cb_t)(bool ok, void *ctx); /* ok: OK chosen; false: cancel, Esc, or the overlay was removed */

/* Confirmation dialog with two buttons. ok_label and cancel_label may be NULL ("OK", "Cancel"). */
nema_err_t pal_dialog_confirm(const char *title, const char *text, const char *ok_label, const char *cancel_label,
                              pal_dialog_cb_t cb, void *ctx);
bool pal_dialog_confirm_wait(const char *title, const char *text, const char *ok_label, const char *cancel_label);

/* Text input: a single-line editor for the physical keyboard. Characters insert, Backspace and Delete remove, Left, Right,
 * Home and End move, Enter is OK, Esc is cancel. mask shows '*' (passwords). The text is at most max_len bytes (ASCII).
 * The callback gets the text on OK; on cancel ok is false and text is "". */
typedef void (*pal_textinput_cb_t)(bool ok, const char *text, void *ctx);
nema_err_t pal_textinput_open(const char *title, const char *initial, int max_len, bool mask, pal_textinput_cb_t cb,
                              void *ctx);
/* Blocking variant: true on OK with the text in out (cap bytes, NUL-terminated). */
bool pal_textinput_wait(const char *title, const char *initial, int max_len, bool mask, char *out, size_t cap);

/* Picker: a list of items (up to PAL_PICKER_ITEMS; the pointers must stay valid until the picker closes). Enter picks
 * (cb gets the index), Esc cancels (cb gets -1). Up and Down (Fn+W/S) move, with wrap-around. */
typedef void (*pal_picker_cb_t)(int index, void *ctx);
nema_err_t pal_picker_open(const char *title, const char *const *items, int n, pal_picker_cb_t cb, void *ctx);

/* Busy overlay: a spinner and a text. It takes every key (the user cannot back out); the caller must hide it. Show again
 * replaces the text. */
void pal_busy_show(const char *text);
void pal_busy_hide(void);
bool pal_busy_showing(void);

#endif
