/* PalanuOS overlays: the stack above the front view and the global key bindings of the home view.
 *
 * An overlay is an ordinary pal_view_t. Pushing it draws it on top of the layer below, which is told through its obscured
 * callback. The top overlay gets the keys first; Esc pops it unless its key handler consumed the key. Popping calls the
 * overlay's leave and tells the layer below through exposed, then the desktop redraws in full. Overlays are pushed and popped
 * from any task: the desktop applies the change on its own task (NEMA_OK means queued). Up to 4 overlays at once.
 */
#ifndef PAL_OVERLAY_H
#define PAL_OVERLAY_H

#include "pal/desktop.h"

#define PAL_OVERLAY_MAX 4

/* Push v. Its flags (PAL_VIEW_OVERLAY, PAL_VIEW_OPAQUE, PAL_VIEW_NOBAR) apply to this push; the view's own flags are kept. */
nema_err_t pal_desktop_overlay_push(pal_view_t *v);
/* Pop the top overlay (leave is called). Does nothing when the stack is empty. */
void pal_desktop_overlay_pop(void);
/* Remove a given overlay from anywhere in the stack (used by the dialogs when they are closed by someone else). */
void pal_desktop_overlay_remove(pal_view_t *v);
pal_view_t *pal_desktop_overlay_top(void); /* NULL when there is none */
size_t pal_desktop_overlay_count(void);

/* Opens the app switcher overlay (the cycle key's tap does the same). Any task. */
void pal_desktop_switcher_show(void);

/* Home key bindings: when the home view is the front layer and no overlay is open, a bound key opens its
 * view as an OPAQUE overlay. Bindings are data set by the boot code; sym is a pal_keysym_t (PAL_KEY_UP, ...). At most 8.
 * view_id NULL removes the binding for sym. */
nema_err_t pal_desktop_bind_home_key(uint8_t sym, const char *view_id);

#endif
