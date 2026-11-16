/* PalanuOS toasts: the shown toast and its timer. Posting goes through the notify toast queue (pal/notify.h), so any
 * task may post. The desktop calls pal_toast_service on each loop: it expires the shown toast and takes the next from the
 * queue. No polling: the desktop sleeps until pal_toast_next_ms. */
#include <string.h>

#include "nema/types.h"
#include "pal/notify.h"
#include "pal/toast.h"

#define TOAST_MS_MIN 100
#define TOAST_MS_MAX 10000

static struct {
    bool on;
    uint32_t end;
    char text[PAL_TOAST_CHARS + 1];
} T;

nema_err_t psys_toast(const char *text, int ms) {
    if (!text) return NEMA_ERR_INVAL;
    if (ms <= 0) ms = PAL_TOAST_MS_DEFAULT;
    if (ms < TOAST_MS_MIN) ms = TOAST_MS_MIN;
    if (ms > TOAST_MS_MAX) ms = TOAST_MS_MAX;
    int32_t r = psys_toast_post("system", text, ms);
    return r < 0 ? (nema_err_t)r : NEMA_OK;
}

bool pal_toast_service(uint32_t now_ms) {
    bool changed = false;
    if (T.on && (int32_t)(now_ms - T.end) >= 0) {
        T.on = false;
        changed = true;
    }
    if (!T.on) {
        psys_toast_t t;
        if (psys_toast_take(&t)) {
            T.on = true;
            T.end = now_ms + (uint32_t)t.ms;
            nema_strlcpy(T.text, t.text, sizeof(T.text));
            changed = true;
        }
    }
    return changed;
}

int32_t pal_toast_next_ms(uint32_t now_ms) {
    if (!T.on) return -1;
    int32_t d = (int32_t)(T.end - now_ms);
    return d < 0 ? 0 : d;
}

bool pal_toast_showing(void) { return T.on; }
const char *pal_toast_text(void) { return T.on ? T.text : ""; }
void pal_toast_reset(void) { memset(&T, 0, sizeof(T)); }
