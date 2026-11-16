/* The tty module of the palanu API (api/palanu/tty.h): the terminal mode, its size, and its capabilities. The app's
 * terminal is its controlling terminal, the first of stdin, stdout and stderr that is a terminal. The mode the app
 * sets is put back when the app ends (api_tty_restore), so a terminal never stays raw after the app that made it raw. */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"

/* The PAL_TTY_* and PAL_TERM_* values of api/palanu/tty.h, as numbers. */
#define MODE_RAW 1
#define MODE_NOSIG 2
#define MODE_NOECHO 4
#define TERM_CAPS_ALL (1 | 2 | 4) /* 256 colours, UTF-8, alternate screen */

/* The first of fd 0, 1, 2 that is a terminal (its size is written to ws when given), or -1. */
static int ctty_fd(nema_winsize_t *ws) {
    for (int fd = 0; fd <= 2; fd++) {
        nema_winsize_t got;
        if (nema_ioctl(fd, NEMA_TIOC_GETSIZE, &got) == NEMA_OK) {
            if (ws) *ws = got;
            return fd;
        }
    }
    return -1;
}

/* Line flags for a PAL mode. RAW and NOSIG are the same here: no echo, no line editing, no keyboard signals. */
static uint32_t line_flags(int32_t mode) {
    if (mode & (MODE_RAW | MODE_NOSIG)) return NEMA_TTY_RAW;
    return (mode & MODE_NOECHO) ? (NEMA_TTY_COOKED & ~NEMA_TTY_ECHO) : NEMA_TTY_COOKED;
}

/* The PAL mode a line-flag state reads as. Raw states without line editing read as RAW. */
static int32_t mode_of(uint32_t flags) {
    if (!(flags & NEMA_TTY_ICANON)) return MODE_RAW;
    return (flags & NEMA_TTY_ECHO) ? 0 : MODE_NOECHO;
}

int32_t psys_api_tty_mode(api_ctx_t *c, int32_t mode) {
    const int32_t known = MODE_RAW | MODE_NOSIG | MODE_NOECHO;
    if (mode < 0 || (mode & ~known)) return NEMA_ERR_INVAL;
    int fd = ctty_fd(NULL);
    if (fd < 0) return NEMA_ERR_NOSYS;
    uint32_t before;
    if (nema_ioctl(fd, NEMA_TIOC_GETMODE, &before) != NEMA_OK) return NEMA_ERR_NOSYS;
    api_ext_t *x = api_ext(c);
    if (!x->tty_saved) { /* the mode the app found: the first change keeps it, later changes do not */
        x->tty_saved = true;
        x->tty_fd = fd;
        x->tty_flags = before;
    }
    uint32_t want = line_flags(mode);
    return nema_ioctl(fd, NEMA_TIOC_SETMODE, &want);
}

int32_t psys_api_tty_get(api_ctx_t *c) {
    (void)c;
    int fd = ctty_fd(NULL);
    uint32_t flags;
    if (fd < 0 || nema_ioctl(fd, NEMA_TIOC_GETMODE, &flags) != NEMA_OK) return NEMA_ERR_NOSYS;
    return mode_of(flags);
}

/* 4 bytes, little-endian: columns, then rows. 80x24 when no terminal reports a size. */
int32_t psys_api_tty_size(api_ctx_t *c, void *buf, uint32_t len) {
    (void)c;
    if (len < 4) return NEMA_ERR_INVAL;
    int cols, rows;
    api_tty_size(&cols, &rows);
    api_put16(buf, (uint32_t)cols);
    api_put16((uint8_t *)buf + 2, (uint32_t)rows);
    return 0;
}

int32_t psys_api_tty_caps(api_ctx_t *c) {
    (void)c;
    return TERM_CAPS_ALL;
}

void api_tty_size(int *cols, int *rows) {
    nema_winsize_t ws;
    *cols = 80;
    *rows = 24;
    if (ctty_fd(&ws) >= 0 && ws.cols && ws.rows) {
        *cols = ws.cols;
        *rows = ws.rows;
    }
}

int api_ctty_fd(void) { return ctty_fd(NULL); }

void api_tty_restore(api_ctx_t *c) {
    api_ext_t *x = api_ext(c);
    if (!x->tty_saved) return;
    x->tty_saved = false;
    nema_ioctl(x->tty_fd, NEMA_TIOC_SETMODE, &x->tty_flags);
}
