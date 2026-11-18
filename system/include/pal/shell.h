/* Shell services (ADR-0010 §5): the dialog, the picker, the text entry, the busy box and the on-screen keyboard. Their behaviour
 * (the state, the keys, the answer to the caller) is the system's and the same under every session; how they look is the running
 * session's. Each service is an overlay that can describe itself as data (pal_shell_view_t); a session that draws the services itself
 * says so in its chrome (`services`) and lays that data out with its own components. The classic session draws them with the
 * theme's components (the overlays' own draw); with no session the system console draws a text version and gives them the keys.
 * Toasts are the same: a session with `services` shows pal_toast_text() itself. Desktop task only. */
#ifndef PAL_SHELL_H
#define PAL_SHELL_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/desktop.h"
#include "pal/input.h"

typedef enum {
    PAL_SHELL_NONE = 0,
    PAL_SHELL_DIALOG,   /* title, text, items = {cancel, ok}, sel = the focused button */
    PAL_SHELL_PICKER,   /* title, items, sel = the focused item */
    PAL_SHELL_TEXT,     /* title, text = the entry (already masked), cursor = the caret (bytes into text) */
    PAL_SHELL_BUSY,     /* text */
    PAL_SHELL_KEYBOARD, /* title, text = the entry (masked when it should be), cursor, keys, focus */
} pal_shell_kind_t;

#define PAL_SHELL_ITEMS 12
#define PAL_SHELL_KEYS 48

typedef struct {
    uint8_t row, x, w; /* half-units of a 20-wide row (pal/vk_layout.h) */
    char label[8];
    bool accent; /* a mode or action key (shift, layer, OK): drawn apart from the letters */
} pal_shell_key_t;

typedef struct pal_shell_view {
    pal_shell_kind_t kind;
    const char *title;
    const char *text;
    const char *items[PAL_SHELL_ITEMS];
    int nitems, sel, cursor;
    pal_shell_key_t keys[PAL_SHELL_KEYS];
    int nkeys, focus, rows;
} pal_shell_view_t;

/* The service in front: the top overlay, when it is one. false when the top overlay is not a service (or there is none). */
bool pal_shell_top(pal_shell_view_t *out);
/* One view: true when v is a service that described itself into out. */
bool pal_shell_describe(pal_view_t *v, pal_shell_view_t *out);
/* The keys go to the service in front (the console and a session use it when they take the keys themselves). false: none in front. */
bool pal_shell_key(const pal_key_t *k);

#endif
