/* paltui: terminal UI toolkit for PalanuOS apps. Immediate mode: the app runs its frame code every time and
 * the widgets write into a back cell buffer; tui_end diffs it against the screen and sends only what changed.
 *
 * Single header (the documented way):
 *     #define PALTUI_IMPLEMENTATION     // in exactly one translation unit
 *     #define PALTUI_WITH_PAL           // optional: tui_io_pal(), the default backend (uses the SDK palanu.h)
 *     #include "paltui/paltui.h"
 * The app is built with `-I sdk` (bun forge/tools/palanu.ts build does that). sdk/paltui/paltui.c is the same
 * implementation as a separate file, for linking. Guide: docs/TUI.md.
 *
 * No heap: tui_t is a plain struct the app declares static. Only the memory the app passes in is ever written. */
#ifndef PALTUI_H
#define PALTUI_H

#include <stdint.h>

#ifndef PALTUI_MAX_COLS
#define PALTUI_MAX_COLS 100
#endif
#ifndef PALTUI_MAX_ROWS
#define PALTUI_MAX_ROWS 40
#endif
#ifndef PALTUI_OUT_BYTES
#define PALTUI_OUT_BYTES 1024 /* output batch: flushed when full and at the end of each tui_end */
#endif
#ifndef PALTUI_MAX_IDS
#define PALTUI_MAX_IDS 32 /* focusable widgets per frame */
#endif
#ifndef PALTUI_SLOTS
#define PALTUI_SLOTS 16 /* per-widget state (cursor, scroll), replaced least recently used */
#endif
#ifndef PALTUI_EXTRA
#define PALTUI_EXTRA 4 /* extra handles tui_wait_extra can watch (PALTUI_WITH_PAL) */
#endif
#define PALTUI_IN_BYTES 128 /* input bytes kept while decoding (one escape sequence or paste chunk) */

#define TUI_RUN 1  /* tui_frame: keep running */
#define TUI_QUIT 0 /* tui_frame: a stop was asked (Ctrl-C, TERM, HUP): the terminal is already restored */

/* Backend: the only thing the library needs from the outside. Every function returns >= 0 on success.
 * read: waits up to timeout_ms (-1 forever, 0 only checks) and returns the bytes read, 0 on timeout, or one of
 * the TUI_IO_WAKE_* codes; any other negative value is a stop. Times are milliseconds (wrap-around is fine). */
typedef struct {
    int32_t (*write)(const void *buf, uint32_t len);
    int32_t (*read)(void *buf, uint32_t cap, int32_t timeout_ms);
    int32_t (*size)(int *cols, int *rows);
    int32_t (*caps)(void);
    int32_t (*now_ms)(void);
} tui_io_t;

#define TUI_IO_WAKE_RESIZE (-100) /* the terminal size changed: query size again */
#define TUI_IO_WAKE_QUIT (-101)   /* a stop was asked */
#define TUI_IO_WAKE_EXTRA (-102)  /* an extra handle (tui_wait_extra) is ready: tui_extra_got tells which */

/* caps bits returned by caps(). 256 wins over 16; neither set means mono (reverse, bold, underline only). */
#define TUI_CAP_256 0x01
#define TUI_CAP_UTF8 0x02
#define TUI_CAP_ALT 0x04
#define TUI_CAP_16 0x10

/* Semantic styles: a cell holds one of these; the SGR for it comes from the caps (256, 16 or mono). */
#define TUI_NORMAL 0
#define TUI_TITLE 1
#define TUI_SEL 2 /* selected row, focused button, status bar */
#define TUI_DIM 3
#define TUI_ERR 4
#define TUI_OK 5
#define TUI_ACCENT 6 /* selection in an unfocused list */
#define TUI_INPUT 7 /* field background */

/* Keys, the same shape GUI apps get later: sym for named keys, cp for characters, mods bits. */
#define TUI_K_NONE 0
#define TUI_K_ENTER 1
#define TUI_K_TAB 2
#define TUI_K_BACKSPACE 3
#define TUI_K_DELETE 4
#define TUI_K_ESC 5
#define TUI_K_UP 6
#define TUI_K_DOWN 7
#define TUI_K_LEFT 8
#define TUI_K_RIGHT 9
#define TUI_K_HOME 10
#define TUI_K_END 11
#define TUI_K_PGUP 12
#define TUI_K_PGDN 13
#define TUI_K_INSERT 14
#define TUI_K_F1 15 /* F1..F12 are TUI_K_F1 + n - 1 */
#define TUI_MOD_SHIFT 1
#define TUI_MOD_ALT 2
#define TUI_MOD_CTRL 4

typedef struct {
    uint16_t sym; /* TUI_K_*, or 0 for a character */
    uint32_t cp;  /* the character (Ctrl-letters: the lower-case letter, with TUI_MOD_CTRL); 0 for named keys */
    uint8_t mods;
} tui_key_t;

/* One cell: 4 bytes. attr flags: TUI_A_WIDE (lead of a double-width character), TUI_A_CONT (its second half),
 * TUI_A_CURSOR (drawn reversed, for the cursor of a field). */
#define TUI_A_WIDE 1
#define TUI_A_CONT 2
#define TUI_A_CURSOR 4
typedef struct {
    uint16_t cp; /* BMP only; anything else is drawn as '?' */
    uint8_t style;
    uint8_t attr;
} tui_cell_t;

typedef struct {
    uint32_t id, pos, scroll, seq; /* hashed widget id, cursor (byte offset), first visible row or byte, LRU stamp */
} tui_slot_t;

typedef struct tui_s {
    tui_io_t io;
    int open, suspended, quit, full, utf8, depth, saved_mode, cols, rows, cur;
    tui_cell_t cells[2][PALTUI_MAX_ROWS * PALTUI_MAX_COLS]; /* cells[cur] is the back buffer, the other the screen */
    int rx, ry, rw, rh, cy;                                 /* current region and the row the next widget uses */
    int ocx, ocy, ostate;                                   /* terminal cursor and SGR state as last emitted */
    uint32_t focus, w_frame, w_last, w_max, seq;
    uint32_t ids[PALTUI_MAX_IDS], prev[PALTUI_MAX_IDS];
    int nids, nprev;
    tui_slot_t slots[PALTUI_SLOTS];
    uint8_t in[PALTUI_IN_BYTES];
    int in_pos, in_len, paste_mode, esc_pending;
    int32_t esc_at;
    int ev, consumed; /* this frame's event: 0 none, 1 key, 2 paste chunk */
    tui_key_t key;
    const char *paste;
    uint32_t paste_len;
    char out[PALTUI_OUT_BYTES];
    int outn;
} tui_t;

/* Lifecycle. tui_open enters the alternate screen, hides the cursor and (PALTUI_WITH_PAL) switches to raw mode. */
int tui_open(tui_t *t, const tui_io_t *io);
void tui_close(tui_t *t);
int tui_suspend(tui_t *t); /* gives the terminal back: main screen, cursor, cooked mode (for an SSH session) */
int tui_resume(tui_t *t);  /* takes it again; the next tui_end repaints everything */
void tui_size(const tui_t *t, int *cols, int *rows);

/* The frame: tui_frame waits up to timeout_ms for input, a resize, or a stop, then clears the back buffer. The app
 * draws with the widgets below and calls tui_end, which emits the changes. Keys: one per frame, Tab moves focus. */
int tui_frame(tui_t *t, int32_t timeout_ms);
void tui_end(tui_t *t);
const tui_key_t *tui_key(tui_t *t);          /* takes this frame's key (NULL if none); widgets see only what is left */
const char *tui_paste(tui_t *t, uint32_t *len); /* takes this frame's bracketed-paste chunk (valid until the next frame) */

/* Layout: widgets stack down the current region. tui_region sets it (and the row to start at); tui_gap skips rows. */
void tui_region(tui_t *t, int x, int y, int w, int h);
void tui_gap(tui_t *t, int rows);
void tui_box(tui_t *t, int x, int y, int w, int h, const char *title); /* a frame at absolute coordinates */
void tui_status(tui_t *t, const char *text);                            /* bottom row, absolute */

/* Widgets. ids are strings (hashed to 32 bits); focused widgets act on keys. Return values are documented per widget. */
void tui_title(tui_t *t, const char *text);
void tui_label(tui_t *t, const char *text);
void tui_text(tui_t *t, const char *text);                                 /* word-wrapped, one or more rows */
int tui_menu(tui_t *t, const char *id, const char *const *items, int n, int *sel); /* 1 when Enter picks a row */
int tui_list(tui_t *t, const char *id, const char *const *items, int n, int *sel);  /* 1 when the selection moved */
int tui_input(tui_t *t, const char *id, char *buf, uint32_t cap); /* bit 1: text changed, bit 2: Enter pressed */
int tui_password(tui_t *t, const char *id, char *buf, uint32_t cap); /* same bits; shows '*' per character */
int tui_button(tui_t *t, const char *id, const char *label); /* 1 when Enter or Space presses it */
int tui_checkbox(tui_t *t, const char *id, const char *label, int *on); /* 1 when toggled */
void tui_progress(tui_t *t, uint32_t value, uint32_t max);

/* Modal helpers: each runs its own loop over the screen as last shown (call them between frames, not inside one).
 * They return when the user answers; a stop returns the default. The next tui_frame repaints the screen. */
int tui_prompt(tui_t *t, const char *title, char *buf, uint32_t cap); /* 1 Enter, 0 Esc */
int tui_confirm(tui_t *t, const char *text);                         /* 1 yes (Y, or Enter on Yes), 0 no */
int tui_pick(tui_t *t, const char *title, const char *const *items, int n); /* index, or -1 on Esc */

/* Stats for budgeting: cell writes by widgets in the last frame and the largest so far. */
void tui_stats(const tui_t *t, uint32_t *last, uint32_t *max);
/* Reads the cell last shown at (x, y), for tests and screenshots. Returns 0, or -1 outside the screen. */
int tui_shown(const tui_t *t, int x, int y, uint32_t *cp, uint8_t *style, uint8_t *attr);

#ifdef PALTUI_WITH_PAL
/* The default backend: stdin and stdout, the terminal size, its capabilities and the clock, through the SDK. */
const tui_io_t *tui_io_pal(void);
/* Extra handles (files, pipes, sockets) that tui_frame also waits for, with PAL_POLL_* bits in want (0 removes).
 * When one is ready tui_frame returns with no key, and tui_extra_got(h) gives the ready bits until the next wait.
 * The app reads the handle itself. Returns 0, or -1 when the table is full. */
int tui_wait_extra(tui_t *t, int32_t h, uint16_t want);
uint16_t tui_extra_got(int32_t h);
#endif


#ifdef PALTUI_IMPLEMENTATION
#ifdef PALTUI_WITH_PAL
#include "../palanu.h"
#endif
/* The implementation lives in three parts next to this header (kept under the 800-line rule); they are one
 * translation unit, so an app still compiles a single C file. */
#include "paltui_core.inc"    /* buffers, output, input decoding, frame, the default backend */
#include "paltui_widgets.inc" /* layout, widgets */
#include "paltui_modal.inc"   /* prompt, confirm, pick */
#endif

#endif /* PALTUI_H */
