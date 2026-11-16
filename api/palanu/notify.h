/* Notifications and toasts (API level 2). A notification has a title (up to 31 characters) and a text (up to 95); the system
 * keeps the last 32 and the notification center shows them. A toast is a short message the system shows on top of the
 * screen. Posting needs the notify permission. An app posts at most 4 notifications or toasts per second; the excess is
 * refused with PAL_ERR_BUSY. Until the notification center exists, notifications are kept and logged, and shown by the
 * notify shell command. */
#ifndef PALANU_API_NOTIFY_H
#define PALANU_API_NOTIFY_H

#include "annot.h"

/* Flags for pal_notify_post, an alert: the system may show it at once, not only in the list. Other bits must be zero. */
#define PAL_NOTIFY_ALERT 0x01

PAL_SINCE(2) PAL_CAP("notify") PAL_WASM(toast)
PAL_DOC("Shows text (NUL-terminated, up to 95 characters) as a toast for ms milliseconds (100 to 10000). Returns 0, PAL_ERR_INVAL, PAL_ERR_BUSY (rate limit), or PAL_ERR_FULL when the toast queue of 4 is full.")
int32_t pal_toast(const char *text, int32_t ms);

PAL_SINCE(2) PAL_CAP("notify") PAL_WASM(notify_post)
PAL_DOC("Posts a notification with title and text (NUL-terminated) and flags (PAL_NOTIFY_*). Returns its id (above 0), PAL_ERR_INVAL, or PAL_ERR_BUSY (rate limit).")
int32_t pal_notify_post(const char *title, const char *text, int32_t flags);

PAL_SINCE(2) PAL_CAP("notify") PAL_WASM(notify_update)
PAL_DOC("Changes the title and text of notification id, which this app posted. Returns 0, PAL_ERR_NOENT (no such id), PAL_ERR_PERM (another app's), or PAL_ERR_INVAL (too long).")
int32_t pal_notify_update(int32_t id, const char *title, const char *text);

PAL_SINCE(2) PAL_CAP("notify") PAL_WASM(notify_cancel)
PAL_DOC("Removes notification id, which this app posted. Returns 0, PAL_ERR_NOENT (no such id), or PAL_ERR_PERM (another app's).")
int32_t pal_notify_cancel(int32_t id);

#endif
