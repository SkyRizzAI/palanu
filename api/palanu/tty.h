/* The terminal the app talks to through stdin and stdout. The app's terminal is its controlling terminal: the first of
 * stdin, stdout and stderr that is a terminal. A mode replaces the one before it:
 *   PAL_TTY_COOKED (0)   lines are edited and echoed, and Ctrl-C stops the app. This is the default.
 *   PAL_TTY_RAW (1)      every byte arrives as it is typed, Ctrl-C included (as byte 0x03; it does not stop the app).
 *   PAL_TTY_NOSIG (2)    identical to RAW on this system: raw input and no signals from the keyboard. Kept for code
 *                        written for terminals where raw and no-signal modes differ.
 *   PAL_TTY_NOECHO (4)   cooked lines without echo, for typing passwords.
 * When the app ends, the terminal goes back to the mode it had before the app first changed it, however the app ended. */
#ifndef PALANU_API_TTY_H
#define PALANU_API_TTY_H

#include "annot.h"

#define PAL_TTY_COOKED 0
#define PAL_TTY_RAW 1
#define PAL_TTY_NOSIG 2
#define PAL_TTY_NOECHO 4

#define PAL_TERM_256COLOR 1
#define PAL_TERM_UTF8 2
#define PAL_TERM_ALTSCREEN 4

PAL_SINCE(1) PAL_WASM(tty_raw) PAL_ALIAS(pal_tty_raw)
PAL_DOC("Sets the terminal mode to a PAL_TTY_* value (any other bit is refused). Returns 0, or a negative error when stdin, stdout and stderr are not a terminal.")
int32_t pal_tty_mode(int32_t mode);

PAL_SINCE(2) PAL_WASM(tty_get)
PAL_DOC("Returns the current terminal mode as a PAL_TTY_* value: RAW for any raw state, NOECHO for cooked without echo. Returns a negative error when there is no terminal.")
int32_t pal_tty_get(void);

PAL_SINCE(1) PAL_WASM(term_size) PAL_ALIAS(pal_term_size)
PAL_DOC("Writes the size of the controlling terminal into buf as 4 bytes: columns, then rows, each a little-endian uint16. Needs len >= 4. 80x24 when no terminal reports a size.")
int32_t pal_tty_size(PAL_OUT(len) void *buf, uint32_t len);

PAL_SINCE(2) PAL_WASM(tty_caps)
PAL_DOC("Returns the terminal capability bits: PAL_TERM_256COLOR (1), PAL_TERM_UTF8 (2), PAL_TERM_ALTSCREEN (4). The on-device terminal has all three.")
int32_t pal_tty_caps(void);

#endif
