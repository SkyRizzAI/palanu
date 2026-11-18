/* App screens (appui.c): the form and canvas service behind api/palanu/ui.h. app is the caller's api context (who the screen is for). */
#ifndef SYSTEM_APPUI_H
#define SYSTEM_APPUI_H

#include <stdbool.h>
#include <stdint.h>

void appui_init(void);          /* boot, after the desktop: registers the screens' views */
void appui_after_render(void);  /* desktop task, after the compositor's frame */
void appui_release(const void *app);

int32_t appui_form_open(const void *app, const char *title);
int32_t appui_form_add(const void *app, int32_t id, int32_t kind, const char *label, const char *value);
int32_t appui_form_set(const void *app, int32_t id, const char *value);
int32_t appui_form_get(const void *app, int32_t id, char *buf, uint32_t len);
int32_t appui_form_clear(const void *app);
int32_t appui_form_next(const void *app, int32_t *rec, int32_t timeout_ms);
int32_t appui_close(const void *app, bool canvas);
int32_t appui_canvas_open(const void *app, const char *title);
int32_t appui_canvas_present(const void *app, const uint8_t *px, uint32_t len, int32_t x, int32_t y, int32_t w);
int32_t appui_canvas_key(const void *app, uint8_t *rec, int32_t timeout_ms);

#endif
