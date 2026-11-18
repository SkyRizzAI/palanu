/* The system console (docs/adr/0010-system-console-and-desktop-sessions.md): the text screen of the system itself, the equivalent of
 * tty1. It shows the boot (service status lines, warnings and errors), then a login shell. It draws with its own canvas, its own
 * fixed colours and a Spleen font, never with the theme, the compositor's frame or a desktop's code, so it works when a desktop
 * does not. Ctrl+Alt+Esc switches between it and the running desktop session; the input layer gives it that key first.
 *
 * Boot: the console is in front from the start. With boot.quiet on, a plain splash covers the log until a key is pressed.
 * When the boot stages are done, a running desktop session takes the screen; with no session (boot.target console, safe mode,
 * or a desktop that failed to start) the console stays and asks for the PIN when one is set, then starts a shell.
 *
 * Every call is a no-op before pal_console_init, so hosts and tests without a screen need nothing. */
#ifndef PAL_CONSOLE_H
#define PAL_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pal/input.h"

/* After the display is up (boot). quiet: the splash covers the log until a key is pressed. */
void pal_console_init(bool quiet);

/* Text for the console from any task: UTF-8 and terminal sequences, "\n" is a new line. */
void pal_console_print(const char *text);
void pal_console_printf(const char *fmt, ...);

bool pal_console_front(void);  /* the console has the screen */
void pal_console_show(void);   /* bring it in front (from any task) */
void pal_console_hide(void);   /* give the screen to the running desktop session, if there is one */
uint32_t pal_console_generation(void); /* goes up every time the console takes the screen: a session redraws everything after it */

/* The screen loop (desktop.c) calls these on its own task. */
bool pal_console_key(const pal_key_t *k);   /* true when the console took the key (its own switch key, or any key while in front) */
void pal_console_pump(uint32_t now_ms);     /* drains queued text and the shell's output; finishes the boot */
void pal_console_render(bool full);         /* draws and presents what changed; only while in front */
int32_t pal_console_next_ms(uint32_t now_ms); /* when the console next needs the loop, or -1 */

#endif
