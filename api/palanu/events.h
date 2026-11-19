/* Waiting and events. pal_wait waits on several handles at once: a file or pipe end, a terminal, or the process event
 * queue from pal_events. Each record of the array is 8 bytes, little-endian: int32 h (the handle), uint16 want (the
 * PAL_POLL_IN and PAL_POLL_OUT bits the app waits for) and uint16 got (what is ready, written by the call). At most 16
 * records. The event queue reports RESIZE (the terminal size changed), QUIT (a stop was asked: Ctrl-C, TERM, HUP), and since
 * 19 November 2026 FOCUS, BLUR and EXPOSE (the app's screen came to the front, left it, or shows again after an overlay), KEY (a
 * key on the app's canvas screen), NOTIFY (a notification was posted, to every app) and SETTING (a setting changed, to every app).
 * Each event is 16 bytes, little-endian: uint32 type, a, b, c. The queue keeps the last 8 posted events; an app that never called
 * pal_events gets none. A stop request ends a wait that does not include the queue, like a read; when the queue is in the wait it
 * becomes ready instead, and reading QUIT takes the request. */
#ifndef PALANU_API_EVENTS_H
#define PALANU_API_EVENTS_H

#include "annot.h"

#define PAL_POLL_IN 0x1
#define PAL_POLL_OUT 0x2
#define PAL_POLL_HUP 0x4
#define PAL_POLL_ERR 0x8

/* Event types. RESIZE: a = columns, b = rows. QUIT: a = the stop request taken (PAL_SIG_INT, PAL_SIG_TERM or PAL_SIG_HUP, see
 * proc). KEY: a = the key symbol, b = the code point, c = the modifiers (as pal_canvas_key gives them). NOTIFY: a = the
 * notification's id. SETTING: a = the FNV-1a 32-bit hash of the key's name (read the settings the app cares about again).
 * FOCUS, BLUR, EXPOSE: no data. */
#define PAL_EV_NONE 0
#define PAL_EV_KEY 1
#define PAL_EV_RESIZE 2
#define PAL_EV_FOCUS 3
#define PAL_EV_BLUR 4
#define PAL_EV_QUIT 5
#define PAL_EV_NOTIFY 6
#define PAL_EV_SETTING 7
#define PAL_EV_EXPOSE 8

PAL_SINCE(2) PAL_WASM(wait)
PAL_DOC("Waits until a record of waitfor (n_bytes / 8 records) is ready, or timeout_ms passes (-1 waits forever, 0 only checks). Writes got into each record. Returns the number of ready records, 0 on timeout, or a negative error (n_bytes not a multiple of 8, more than 16 records, or a handle the app does not hold).")
int32_t pal_wait(PAL_INOUT(n_bytes) void *waitfor, uint32_t n_bytes, int32_t timeout_ms);

PAL_SINCE(2) PAL_WASM(events)
PAL_DOC("Returns the process event queue as a handle. It becomes readable when an event is pending; read the events with pal_event_next. Returns a negative error when the app has no free handle.")
int32_t pal_events(void);

PAL_SINCE(2) PAL_WASM(event_next)
PAL_DOC("Reads the next event into ev (16 bytes; len must be at least 16). Returns 16 when an event was written, 0 when timeout_ms passed without one (-1 waits forever, 0 only checks), or a negative error.")
int32_t pal_event_next(PAL_OUT(len) void *ev, uint32_t len, int32_t timeout_ms);

#endif
