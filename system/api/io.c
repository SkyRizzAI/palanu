/* The io module of the palanu API (api/palanu/io.h): pal_write and pal_read. Plain C over the nema handles; the
 * WAMR glue (api_wasm_glue.c, generated) checks the capability and calls these with the app's context. */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"

/* A handle an app may use without opening it: its stdin, stdout and stderr, and the ones it opened itself. */
static bool fd_ok(const api_ctx_t *c, int32_t fd) { return (fd >= 0 && fd <= 2) || api_fd_owned(c, fd); }

/* A canceled or interrupted call ends the app: the runtime is told to stop at the return. */
static int32_t cut(api_ctx_t *c, int32_t n) {
    if (n == NEMA_ERR_CANCELED || n == NEMA_ERR_INTR) api_stop(c, "interrupted");
    return n;
}

int32_t psys_api_write(api_ctx_t *c, int32_t fd, const void *buf, uint32_t len) {
    if (!fd_ok(c, fd)) return NEMA_ERR_BADF;
    return cut(c, nema_write(fd, buf, len));
}

int32_t psys_api_read(api_ctx_t *c, int32_t fd, void *buf, uint32_t len) {
    if (!fd_ok(c, fd)) return NEMA_ERR_BADF;
    for (;;) { /* wake every 100 ms so Ctrl-C and kill are noticed while the app waits for input */
        int n = nema_read_timeout(fd, buf, len, 100);
        if (n != NEMA_ERR_TIMEOUT && n != NEMA_ERR_AGAIN) return cut(c, n);
        if (n == NEMA_ERR_AGAIN && nema_obj_is_nonblock(nema_handle_obj(fd))) return n; /* pal_set_nonblock: the app polls */
        if (nema_interrupted() || nema_canceled()) {
            api_stop(c, "interrupted");
            return NEMA_ERR_INTR;
        }
    }
}
