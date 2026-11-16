/* Notifications and toasts, system side (api/palanu/notify.h). A ring keeps the last 32 notifications and a queue
 * holds 4 toasts. Nothing draws them yet: the notification center and the toast overlay come later. Until then
 * the notify shell command lists them, and each post is logged. A sink lets the UI layer subscribe. */
#ifndef PAL_NOTIFY_H
#define PAL_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>

#define PSYS_NOTIFY_RING 32
#define PSYS_NOTIFY_TITLE 32 /* bytes, NUL included: 31 characters */
#define PSYS_NOTIFY_TEXT 96  /* bytes, NUL included: 95 characters */
#define PSYS_TOAST_QUEUE 4
#define PSYS_SOURCE_MAX 24 /* an app id, NUL included */
#define PSYS_NOTIFY_ALERT 0x1 /* the flag PAL_NOTIFY_ALERT */

typedef struct {
    uint32_t id; /* above 0, never reused while the system runs */
    char source[PSYS_SOURCE_MAX];
    char title[PSYS_NOTIFY_TITLE];
    char text[PSYS_NOTIFY_TEXT];
    uint32_t time; /* calendar seconds at the post (pal_time_epoch) */
    int32_t flags;
    bool read; /* listed by the shell (psys_notify_mark_all_read) */
} psys_notify_t;

typedef struct {
    char source[PSYS_SOURCE_MAX];
    char text[PSYS_NOTIFY_TEXT];
    int32_t ms;
} psys_toast_t;

typedef enum {
    PSYS_NOTIFY_POSTED,
    PSYS_NOTIFY_UPDATED,
    PSYS_NOTIFY_CANCELED, /* n is the notification removed; NULL when the whole list was cleared */
    PSYS_NOTIFY_TOASTED,  /* n is NULL: take the toast with psys_toast_take */
} psys_notify_change_t;

/* One sink (the UI). It runs after each change, outside the lock, so it may read the ring. A new sink replaces the old. */
typedef void (*psys_notify_sink_t)(psys_notify_change_t change, const psys_notify_t *n);
void psys_notify_set_sink(psys_notify_sink_t sink);

/* The operations behind the palanu calls and the shell. source is the posting app's id ("system" for native code).
 * Each returns the id (post), 0, or a negative error: INVAL, NOENT, PERM (another app's notification), BUSY (rate limit:
 * 4 posts or toasts per second per app), FULL (toast queue). */
int32_t psys_notify_post(const char *source, const char *title, const char *text, int32_t flags);
int32_t psys_notify_update(const char *source, int32_t id, const char *title, const char *text);
int32_t psys_notify_cancel(const char *source, int32_t id);
int32_t psys_toast_post(const char *source, const char *text, int32_t ms);

int psys_notify_count(void);
int psys_notify_unread_count(void); /* the ring entries not yet listed (the status bar dot) */
int psys_notify_list(psys_notify_t *out, int cap); /* copies up to cap notifications, newest first; returns how many */
void psys_notify_mark_all_read(void);
void psys_notify_clear(void);
int psys_toast_take(psys_toast_t *out); /* the oldest toast: 1 with it, or 0 when the queue is empty */

#endif
