/* The ui module of the palanu API (api/palanu/ui.h): the app's form or canvas screen, kept by system/src/appui.c. */
#include <stdint.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "../src/appui.h"
#include "nema/nema.h"

int32_t psys_api_form_open(api_ctx_t *c, const char *title) { return appui_form_open(c, title); }
int32_t psys_api_form_add(api_ctx_t *c, int32_t id, int32_t kind, const char *label, const char *value) {
    return appui_form_add(c, id, kind, label, value);
}
int32_t psys_api_form_set(api_ctx_t *c, int32_t id, const char *value) { return appui_form_set(c, id, value); }
int32_t psys_api_form_get(api_ctx_t *c, int32_t id, void *buf, uint32_t len) { return appui_form_get(c, id, buf, len); }
int32_t psys_api_form_clear(api_ctx_t *c) { return appui_form_clear(c); }

int32_t psys_api_form_next(api_ctx_t *c, void *rec, uint32_t len, int32_t timeout_ms) {
    if (!rec || len < 12) return NEMA_ERR_INVAL;
    int32_t e[3];
    int32_t r = appui_form_next(c, e, timeout_ms);
    if (r == 1)
        for (int i = 0; i < 3; i++) {
            uint8_t *p = (uint8_t *)rec + 4 * i;
            p[0] = (uint8_t)e[i], p[1] = (uint8_t)(e[i] >> 8), p[2] = (uint8_t)(e[i] >> 16), p[3] = (uint8_t)(e[i] >> 24);
        }
    return r;
}

int32_t psys_api_form_close(api_ctx_t *c) { return appui_close(c, false); }
int32_t psys_api_canvas_open(api_ctx_t *c, const char *title) { return appui_canvas_open(c, title); }
int32_t psys_api_canvas_present(api_ctx_t *c, const void *px, uint32_t len, int32_t x, int32_t y, int32_t w) {
    if (!px) return NEMA_ERR_INVAL;
    return appui_canvas_present(c, px, len, x, y, w);
}
int32_t psys_api_canvas_key(api_ctx_t *c, void *rec, uint32_t len, int32_t timeout_ms) {
    if (!rec || len < 8) return NEMA_ERR_INVAL;
    return appui_canvas_key(c, rec, timeout_ms);
}
int32_t psys_api_canvas_close(api_ctx_t *c) { return appui_close(c, true); }

void api_ui_release(api_ctx_t *c) { appui_release(c); }
