/* PalanuOS terminal emulator: an xterm-compatible subset sufficient for shells, vim, htop, tmux.
 *
 * Why our own: cells are 8 bytes (libvterm needs ~36), the whole screen plus alternate screen fits a few
 * dozen KB in PSRAM, and the parser is the part SSH sessions depend on, so it is covered by tests that
 * compare against recorded program output. Advertise TERM=xterm-256color.
 *
 * Supported: cursor movement, erase, insert/delete (chars, lines), scroll regions, tabs, SGR with 16/256/RGB
 * colours, alternate screen (47/1047/1049), autowrap, origin mode, application cursor keys, bracketed paste,
 * DEC special graphics, UTF-8, scrollback, DSR/DA replies. Parsed and ignored: mouse modes, titles, sixel.
 */
#ifndef PAL_TERM_H
#define PAL_TERM_H

#include "nema/mem.h"
#include "pal/gfx.h"

#define PAL_TERM_ATTR_BOLD 0x01u
#define PAL_TERM_ATTR_DIM 0x02u
#define PAL_TERM_ATTR_ITALIC 0x04u
#define PAL_TERM_ATTR_UNDERLINE 0x08u
#define PAL_TERM_ATTR_BLINK 0x10u
#define PAL_TERM_ATTR_REVERSE 0x20u
#define PAL_TERM_ATTR_HIDDEN 0x40u
#define PAL_TERM_ATTR_STRIKE 0x80u
#define PAL_TERM_ATTR_WIDE 0x100u      /* the first cell of a two-column character (CJK, emoji) */
#define PAL_TERM_ATTR_WIDE_CONT 0x200u /* the second cell of it: no character of its own (ch 0) */

typedef struct {
    uint32_t ch;
    uint8_t fg, bg; /* palette indices; PAL_C_TERM_FG / PAL_C_TERM_BG are the defaults */
    uint16_t attr;
} pal_cell_t;

#define PAL_TERM_MAX_ROWS 64
#define PAL_TERM_MAX_COLS 128

typedef struct pal_term pal_term_t;
typedef void (*pal_term_reply_fn)(void *ctx, const char *bytes, size_t n);

pal_term_t *pal_term_create(int cols, int rows, int scrollback_lines, nema_owner_t owner);
void pal_term_destroy(pal_term_t *t);

void pal_term_write(pal_term_t *t, const void *data, size_t n); /* feed process output */
void pal_term_resize(pal_term_t *t, int cols, int rows);
void pal_term_set_reply(pal_term_t *t, pal_term_reply_fn fn, void *ctx); /* answers to DSR/DA queries */
void pal_term_reset(pal_term_t *t);

int pal_term_cols(const pal_term_t *t);
int pal_term_rows(const pal_term_t *t);
const pal_cell_t *pal_term_cell(const pal_term_t *t, int col, int row); /* as shown, honouring scrollback view */
void pal_term_cursor(const pal_term_t *t, int *col, int *row, bool *visible);
bool pal_term_app_cursor_keys(const pal_term_t *t);
bool pal_term_bracketed_paste(const pal_term_t *t);
bool pal_term_alt_screen(const pal_term_t *t);
uint32_t pal_term_bell_count(const pal_term_t *t);

/* Scrollback: positive delta scrolls back in history, negative returns toward the live screen. */
void pal_term_scroll_view(pal_term_t *t, int delta);
int pal_term_view_offset(const pal_term_t *t);
int pal_term_scrollback_used(const pal_term_t *t);
/* Frees the scrollback of every live terminal (the memory reclaimer calls this under pressure). Returns the bytes
 * freed; 0 when a terminal is busy right now, in which case nothing is freed and the caller may try again later. */
size_t pal_term_trim_history(void);

/* Drawing. Only rows that changed since the last call are drawn; returns the number of rows drawn. */
void pal_term_mark_all(pal_term_t *t);
void pal_term_mark_row(pal_term_t *t, int row);
int pal_term_draw(pal_term_t *t, pal_canvas_t *c, const pal_font_t *font, int x, int y, bool cursor_on);

/* Plain text of one visible row (trailing spaces trimmed). For tests and the `text` clipboard. */
size_t pal_term_row_text(const pal_term_t *t, int row, char *out, size_t cap);

#endif
