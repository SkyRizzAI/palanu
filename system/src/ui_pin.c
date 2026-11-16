/* PIN entry for the lock screen: a pure state machine. Digits add, Backspace removes, Esc clears, Enter submits. The check
 * function is passed in, so the host tests use a fake and the firmware uses the stored hash (ui_prefs.c). */
#include <string.h>

#include "pal/ui.h"

void pal_ui_pin_reset(pal_ui_pin_t *p) { memset(p, 0, sizeof(*p)); }

static pal_ui_pin_event_t submit(pal_ui_pin_t *p, bool (*check)(const char *)) {
    if (p->len < PAL_UI_PIN_MIN) return PAL_UI_PIN_SHORT;
    if (check && check(p->digits)) {
        pal_ui_pin_reset(p);
        return PAL_UI_PIN_OK;
    }
    p->wrong++;
    p->len = 0;
    p->digits[0] = 0;
    p->last_wrong = true;
    return PAL_UI_PIN_WRONG;
}

pal_ui_pin_event_t pal_ui_pin_key(pal_ui_pin_t *p, const pal_key_t *k, bool (*check)(const char *)) {
    switch (k->sym) {
    case PAL_KEY_CHAR:
        if (k->cp < '0' || k->cp > '9') return PAL_UI_PIN_NONE;
        if (p->len >= PAL_UI_PIN_MAX) return PAL_UI_PIN_NONE;
        p->digits[p->len++] = (char)k->cp;
        p->digits[p->len] = 0;
        p->last_wrong = false;
        return PAL_UI_PIN_EDITED;
    case PAL_KEY_BACKSPACE:
        if (p->len == 0) return PAL_UI_PIN_NONE;
        p->digits[--p->len] = 0;
        p->last_wrong = false;
        return PAL_UI_PIN_EDITED;
    case PAL_KEY_ESC:
        if (p->len == 0) return PAL_UI_PIN_NONE;
        p->len = 0;
        p->digits[0] = 0;
        return PAL_UI_PIN_EDITED;
    case PAL_KEY_ENTER:
        return submit(p, check);
    default:
        return PAL_UI_PIN_NONE;
    }
}
