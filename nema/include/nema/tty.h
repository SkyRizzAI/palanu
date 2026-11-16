/* Nema TTY: the junction between a process and a terminal.
 *
 * Two sides. The process side looks like any handle (read, write, ioctl). The terminal side is whoever
 * shows the output and produces the input: the on-device emulator, a Palanu Link channel, or a UART.
 * Because the process only sees the TTY, those three are interchangeable.
 *
 * Cooked mode: echo, line editing (DEL, Ctrl-U), Ctrl-C raises INT for the foreground process, Ctrl-D ends
 * input, and "\n" in output becomes "\r\n". Raw mode: every byte passes through untouched in both directions.
 */
#ifndef NEMA_TTY_H
#define NEMA_TTY_H

#include "nema/obj.h"
#include "nema/proc.h"

typedef struct nema_tty nema_tty_t;

typedef struct {
    uint16_t cols, rows;
} nema_winsize_t;

#define NEMA_TTY_ECHO 0x1u
#define NEMA_TTY_ONLCR 0x2u  /* output: "\n" -> "\r\n" */
#define NEMA_TTY_ISIG 0x4u   /* Ctrl-C raises INT for the foreground process */
#define NEMA_TTY_ICANON 0x8u /* line editing and line-at-a-time delivery */
#define NEMA_TTY_COOKED (NEMA_TTY_ECHO | NEMA_TTY_ONLCR | NEMA_TTY_ISIG | NEMA_TTY_ICANON)
#define NEMA_TTY_RAW 0x0u

/* ioctl requests on the process side */
#define NEMA_TIOC_GETSIZE 0x7401u /* arg: nema_winsize_t* */
#define NEMA_TIOC_SETMODE 0x7402u /* arg: uint32_t* of NEMA_TTY_* flags */
#define NEMA_TIOC_GETMODE 0x7403u /* arg: uint32_t* */
#define NEMA_TIOC_SETFG 0x7404u   /* arg: nema_pid_t* foreground process group (receives INT and WINCH) */
#define NEMA_TIOC_SETEDIT 0x7405u /* arg: nema_edit_t*: the line the shell is editing now (sh_readline publishes it) */
#define NEMA_TIOC_CLREDIT 0x7406u /* no arg: no line is being edited (submitted, or a program took the terminal) */
#define NEMA_TIOC_SETLEGEND 0x7407u /* arg: const char* text for the terminal side to show as key legends; NULL or "" clears */

#define NEMA_TTY_LEGEND_MAX 160 /* bytes of legend text kept, the NUL included; longer text is refused */

#define NEMA_TTY_EDIT_MAX 256
typedef struct {
    const char *utf8;
    size_t len;
    size_t cursor; /* byte offset into utf8 */
} nema_edit_t;

nema_err_t nema_tty_create(nema_tty_t **out, uint16_t cols, uint16_t rows, nema_owner_t owner);
void nema_tty_destroy(nema_tty_t *t); /* closes the terminal side; the process side sees end of stream */

/* The object to install as stdio of processes. Borrowed; valid until nema_tty_destroy. */
nema_obj_t *nema_tty_proc_obj(nema_tty_t *t);
/* The object a terminal waits on: IN when output is ready for nema_tty_output_read(). */
nema_obj_t *nema_tty_term_obj(nema_tty_t *t);

/* Terminal side. */
int nema_tty_input(nema_tty_t *t, const void *bytes, size_t n);                 /* keys in; returns bytes taken */
int nema_tty_output_read(nema_tty_t *t, void *buf, size_t n, int32_t timeout_ms); /* process output out */
void nema_tty_set_size(nema_tty_t *t, uint16_t cols, uint16_t rows);           /* raises WINCH for the foreground */
void nema_tty_set_foreground(nema_tty_t *t, nema_pid_t pgid);

/* The line being edited, for the terminal side (the on-screen keyboard shows it). The shell publishes it with
 * NEMA_TIOC_SETEDIT; it is cleared when the line is submitted, when a program takes the terminal in raw mode, or with
 * NEMA_TIOC_CLREDIT. Copies are cut at a UTF-8 boundary to NEMA_TTY_EDIT_MAX bytes. */
void nema_tty_set_edit(nema_tty_t *t, const char *utf8, size_t len, size_t cursor);
int nema_tty_get_edit(nema_tty_t *t, char *out, size_t cap, size_t *cursor); /* bytes copied; -1 when no line is edited */
void nema_tty_clear_edit(nema_tty_t *t);

/* The key legends the program on the process side published with NEMA_TIOC_SETLEGEND (the system's terminal parses and
 * shows them; the kernel only keeps the text). Copies the text into out (cut to cap, "" when none) and returns a counter
 * that changes on every SETLEGEND, so the terminal side redraws only when it changed. */
uint32_t nema_tty_get_legend(nema_tty_t *t, char *out, size_t cap);

#endif
