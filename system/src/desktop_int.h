/* Internal to the system: the pieces of the desktop shared by desktop.c, overlay.c, toast.c, dialog.c, picker.c,
 * textinput.c, busy.c, statusitems.c and overlay_switcher.c. Apps and screens use the pal/ headers instead. */
#ifndef SYSTEM_DESKTOP_INT_H
#define SYSTEM_DESKTOP_INT_H

#include "pal/desktop.h"
#include "pal/overlay.h"

/* The desktop task, the cross-task queue, and the state the overlays read. */
bool desk_on_task(void);                       /* true on the desktop task */
bool desk_started(void);                       /* the desktop task exists */
nema_err_t desk_post(void (*fn)(void *arg), void *arg); /* run fn on the desktop task; NEMA_ERR_FULL when the queue is full */
void desk_lock(void);                          /* guards the dialog pools; never held across a call into the desktop */
void desk_unlock(void);
pal_view_t *desk_front(void);                  /* the front view, not the overlays */
int desk_mru_count(void);                      /* views that were in front, most recent first */
pal_view_t *desk_mru_at(int i);
void desk_open_now(const char *id);            /* desktop task only: closes the overlays, then opens the view */
const char *desk_home_id(void);
void desk_wake(void);                          /* wake the desktop loop */
bool desk_locked_view(const pal_view_t *v); /* v is the lock view and the device is locked */

/* Sessions (desktops.c, ADR-0010). desk_session_scope(id) makes what is registered until desk_session_scope(NULL) the session's;
 * desk_session_reset(id) takes all of it away (desktop task only) and clears what the session set on the compositor. */
void desk_session_scope(const char *id);
void desk_session_reset(const char *id);
size_t desk_session_views(const char *id);

/* Overlay stack (overlay.c). Desktop task only. */
int ovl_count(void);
pal_view_t *ovl_view(int i);
uint32_t ovl_flags(int i);                     /* the flags this overlay was pushed with */
pal_view_t *ovl_top(void);
int ovl_base(void);                            /* index of the topmost opaque overlay, -1 when none */
nema_err_t ovl_push_now(pal_view_t *v, uint32_t extra_flags);
void ovl_remove_now(pal_view_t *v);            /* leave is called; the layer below gets exposed */
void ovl_clear_now(void);

/* Status items from statusitems.c; registered by the desktop at init. */
size_t pal_status_builtin_items(const pal_status_item_t **out);

/* App switcher (overlay_switcher.c): opens the switcher overlay. Desktop task only. */
nema_err_t pal_switcher_open(void);


#endif
