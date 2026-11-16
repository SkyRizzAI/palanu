#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nema/log.h"
#include "nema/port.h"
#include "nema/types.h"

const char *nema_err_str(int err) {
    switch (err) {
    case NEMA_OK: return "ok";
    case NEMA_ERR_NOMEM: return "out of memory";
    case NEMA_ERR_INVAL: return "invalid argument";
    case NEMA_ERR_NOENT: return "not found";
    case NEMA_ERR_BUSY: return "busy";
    case NEMA_ERR_TIMEOUT: return "timed out";
    case NEMA_ERR_INTR: return "interrupted";
    case NEMA_ERR_AGAIN: return "would block";
    case NEMA_ERR_PIPE: return "broken pipe";
    case NEMA_ERR_PERM: return "permission denied";
    case NEMA_ERR_NOSYS: return "not implemented";
    case NEMA_ERR_EXIST: return "already exists";
    case NEMA_ERR_IO: return "i/o error";
    case NEMA_ERR_RANGE: return "out of range";
    case NEMA_ERR_BADF: return "bad handle";
    case NEMA_ERR_FULL: return "table full";
    case NEMA_ERR_STATE: return "wrong state";
    case NEMA_ERR_NOSPC: return "no space";
    case NEMA_ERR_ISDIR: return "is a directory";
    case NEMA_ERR_NOTDIR: return "not a directory";
    case NEMA_ERR_PROTO: return "protocol error";
    case NEMA_ERR_UNSUPPORTED: return "unsupported";
    case NEMA_ERR_CANCELED: return "canceled";
    case NEMA_ERR_AUTH: return "authentication failed";
    case NEMA_ERR_REFUSED: return "connection refused";
    case NEMA_ERR_RESET: return "connection reset";
    case NEMA_ERR_UNREACH: return "host or network unreachable";
    default: return "unknown error";
    }
}

void nema_panic(const char *fmt, ...) {
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* Straight to the console: the log ring may be the thing that is broken. */
    nema_port_console_write("\r\nPANIC: ", 9);
    nema_port_console_write(msg, strlen(msg));
    nema_port_console_write("\r\n", 2);
    nema_port_abort();
}
