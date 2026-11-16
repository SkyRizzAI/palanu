#include <stdio.h>
#include <string.h>

#include "libapp.h"
#include "nema/tty.h"

static int out_fd(int fd, const char *fmt, va_list ap) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    return nema_write(fd, buf, (size_t)n);
}

int pal_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = out_fd(1, fmt, ap);
    va_end(ap);
    return r;
}

int pal_eprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = out_fd(2, fmt, ap);
    va_end(ap);
    return r;
}

int pal_puts(const char *s) { return nema_write(1, s, strlen(s)); }

bool pal_isatty(int fd) {
    nema_winsize_t ws;
    return nema_ioctl(fd, NEMA_TIOC_GETSIZE, &ws) == NEMA_OK;
}

void pal_term_size(int *cols, int *rows) {
    nema_winsize_t ws;
    if (nema_ioctl(1, NEMA_TIOC_GETSIZE, &ws) == NEMA_OK) {
        *cols = ws.cols;
        *rows = ws.rows;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

const char *pal_strerror(int err) { return nema_err_str(err); }

char *pal_read_all(int fd, size_t *len) {
    nema_proc_t *p = nema_self();
    nema_owner_t owner = p ? p->owner : NEMA_OWNER_KERNEL;
    size_t cap = 512, n = 0;
    char *buf = (char *)nema_alloc(NEMA_MEM_ANY, owner, cap);
    if (!buf) return NULL;
    for (;;) {
        if (n == cap) {
            char *nb = (char *)nema_alloc(NEMA_MEM_ANY, owner, cap * 2);
            if (!nb) {
                nema_free(buf);
                return NULL;
            }
            memcpy(nb, buf, n);
            nema_free(buf);
            buf = nb;
            cap *= 2;
        }
        int r = nema_read(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    *len = n;
    return buf;
}

int pal_read_password(char *out, size_t cap) {
    uint32_t mode = NEMA_TTY_ONLCR | NEMA_TTY_ISIG | NEMA_TTY_ICANON; /* line input without echo */
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    int n = nema_read(0, out, cap - 1);
    mode = NEMA_TTY_COOKED;
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    pal_puts("\n");
    if (n <= 0) return -1;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    out[n] = 0;
    return n;
}
