/* Events for apps (system/api/events.c, api/palanu/events.h): the system posts them to the event queues of the apps. Internal to
 * the system. An app without a queue (it never called pal_events) gets none; a full queue drops its oldest event. */
#ifndef SYSTEM_APP_EVENTS_H
#define SYSTEM_APP_EVENTS_H

#include <stdint.h>

/* To every app's queue: NOTIFY (a = the notification's id), SETTING (a = FNV-1a 32 of the key). Any task. */
void psys_events_post_all(uint32_t type, uint32_t a, uint32_t b, uint32_t c);
/* To one app's queue; app is its API context (api_ctx_t *). FOCUS, BLUR, EXPOSE, KEY. Any task. */
void psys_events_post_app(const void *app, uint32_t type, uint32_t a, uint32_t b, uint32_t c);
uint32_t psys_events_key_hash(const char *key); /* FNV-1a 32, what SETTING carries */

#endif
