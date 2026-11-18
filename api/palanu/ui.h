/* App screens (API level 6, ADR-0010 §8). An app that is not a terminal app shows one screen of one of two kinds:
 *
 * A form is data: a list of fields (sections, information, switches, choices, sliders, actions, editable text), each with an id the
 * app picks. The running desktop session lays it out in its own look, so the same app looks native under every desktop. The app
 * waits for events (a field changed, an action ran, the screen closed) with pal_form_next.
 *
 * A canvas is pixels: an RGB565 area the size the system gives (below the session's bar), which looks the same under every desktop.
 * The app sends rows of pixels with pal_canvas_present and reads keys with pal_canvas_key.
 *
 * An app has at most one screen at a time; opening one brings it to the front. The screen goes when the app closes it or ends. The
 * records are little endian. No permission is needed. */
#ifndef PALANU_API_UI_H
#define PALANU_API_UI_H

#include "annot.h"

/* Field kinds for pal_form_add, and what their value text is. SECTION: a header over the fields below it (value unused). INFO: a label
 * and a read-only value. TOGGLE: a switch, "0" or "1". CHOICE: one of up to 8, "first|second|third" (the first is chosen). SLIDER:
 * a number, "min|max|step|value". ACTION: a row that does something when chosen (value unused). TEXT: editable text up to 63 bytes,
 * the value is the initial text. */
#define PAL_FORM_SECTION 1
#define PAL_FORM_INFO 2
#define PAL_FORM_TOGGLE 3
#define PAL_FORM_CHOICE 4
#define PAL_FORM_SLIDER 5
#define PAL_FORM_ACTION 6
#define PAL_FORM_TEXT 7

/* Event kinds in a pal_form_next record ({int32 id, int32 kind, int32 value}). CHANGED: a toggle, choice, slider or text changed; value
 * is the new value (0 or 1, the index, the number; 0 for a text). ACTIVATED: an action row was chosen. CLOSED: the person left the
 * screen (Esc, home); the form stays the app's until it closes or reopens it. */
/* Key symbols in a pal_canvas_key record (the system's own numbering): a printable character is PAL_SYM_CHAR with its code point. */
#define PAL_SYM_CHAR 1
#define PAL_SYM_ENTER 2
#define PAL_SYM_ESC 3
#define PAL_SYM_BACKSPACE 4
#define PAL_SYM_TAB 5
#define PAL_SYM_DELETE 6
#define PAL_SYM_UP 7
#define PAL_SYM_DOWN 8
#define PAL_SYM_LEFT 9
#define PAL_SYM_RIGHT 10

#define PAL_FORM_CHANGED 1
#define PAL_FORM_ACTIVATED 2
#define PAL_FORM_CLOSED 3

PAL_SINCE(6) PAL_WASM(form_open)
PAL_DOC("Opens the app's form screen with title (up to 31 bytes) and brings it to the front, with no fields. Returns 0, PAL_ERR_BUSY (the app has a canvas open, or every screen slot is taken), or PAL_ERR_UNSUPPORTED when there is no screen.")
int32_t pal_form_open(const char *title);

PAL_SINCE(6) PAL_WASM(form_add)
PAL_DOC("Adds a field at the end of the form: id (0 to 65535, unique in the form), kind (PAL_FORM_*), label (up to 31 bytes) and value (its text, as each kind says, up to 95 bytes). Returns 0, PAL_ERR_INVAL, PAL_ERR_EXIST (the id is taken), PAL_ERR_FULL (32 fields), or PAL_ERR_NOENT (no form open).")
int32_t pal_form_add(int32_t id, int32_t kind, const char *label, const char *value);

PAL_SINCE(6) PAL_WASM(form_set)
PAL_DOC("Sets the value of field id as its kind reads it: the text of an info or text field, 0 or 1 for a toggle, the index for a choice, the number for a slider. Returns 0, PAL_ERR_NOENT, or PAL_ERR_INVAL.")
int32_t pal_form_set(int32_t id, const char *value);

PAL_SINCE(6) PAL_WASM(form_get)
PAL_DOC("Copies the value of field id as text (NUL-terminated) into buf. Returns its length, PAL_ERR_NOENT, or PAL_ERR_INVAL (buf too small).")
int32_t pal_form_get(int32_t id, PAL_OUT(len) void *buf, uint32_t len);

PAL_SINCE(6) PAL_WASM(form_clear)
PAL_DOC("Removes every field of the app's form (to build it again). Returns 0 or PAL_ERR_NOENT.")
int32_t pal_form_clear(void);

PAL_SINCE(6) PAL_WASM(form_next)
PAL_DOC("Waits up to timeout_ms (-1 for ever) for the next event and copies it into rec ({int32 id, int32 kind, int32 value}, 12 bytes). Returns 1 with an event, 0 on timeout, PAL_ERR_NOENT (no form), or PAL_ERR_INVAL (rec under 12 bytes).")
int32_t pal_form_next(PAL_OUT(len) void *rec, uint32_t len, int32_t timeout_ms);

PAL_SINCE(6) PAL_WASM(form_close)
PAL_DOC("Closes the app's form screen. Returns 0 or PAL_ERR_NOENT.")
int32_t pal_form_close(void);

PAL_SINCE(6) PAL_WASM(canvas_open)
PAL_DOC("Opens the app's canvas screen with title and brings it to the front. Returns its size as (width << 16) | height in pixels, PAL_ERR_BUSY (the app has a form open, or every screen slot is taken), PAL_ERR_NOMEM, or PAL_ERR_UNSUPPORTED (no full-colour screen).")
int32_t pal_canvas_open(const char *title);

PAL_SINCE(6) PAL_WASM(canvas_present)
PAL_DOC("Shows rows of w RGB565 pixels (2 bytes each, little endian) from px at x, y on the canvas; len is a whole number of rows. Returns 0, PAL_ERR_INVAL (outside the canvas, or len not whole rows), or PAL_ERR_NOENT (no canvas).")
int32_t pal_canvas_present(PAL_IN(len) const void *px, uint32_t len, int32_t x, int32_t y, int32_t w);

PAL_SINCE(6) PAL_WASM(canvas_key)
PAL_DOC("Waits up to timeout_ms (-1 for ever) for a key on the canvas and copies it into rec ({uint8 sym, uint8 mods, uint16 0, uint32 code point}, 8 bytes; sym as pal_key events). Returns 1 with a key, 0 on timeout, 2 when the person left the screen, PAL_ERR_NOENT, or PAL_ERR_INVAL.")
int32_t pal_canvas_key(PAL_OUT(len) void *rec, uint32_t len, int32_t timeout_ms);

PAL_SINCE(6) PAL_WASM(canvas_close)
PAL_DOC("Closes the app's canvas screen. Returns 0 or PAL_ERR_NOENT.")
int32_t pal_canvas_close(void);

#endif
