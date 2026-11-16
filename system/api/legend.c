/* The legend module of the palanu API (api/palanu/legend.h). The app's text goes to its controlling terminal
 * (NEMA_TIOC_SETLEGEND); the Terminal app reads it back, parses it (pal_legend_parse) and shows it in the legend bar while
 * that terminal is in front. Text is checked here so a malformed one is refused at the call, not dropped later. The
 * legends are removed when the app ends (api_legend_release). */
#include <stdint.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"
#include "pal/legend.h"

int32_t psys_api_legend_show(api_ctx_t *c, const char *spec) {
    if (!spec) spec = "";
    pal_legend_item_t items[PAL_LEGEND_MAX];
    char buf[NEMA_TTY_LEGEND_MAX];
    if (pal_legend_parse(spec, items, PAL_LEGEND_MAX, buf, sizeof(buf)) < 0) return NEMA_ERR_INVAL;
    int fd = api_ctty_fd();
    if (fd < 0) return NEMA_ERR_NOSYS;
    nema_err_t r = nema_ioctl(fd, NEMA_TIOC_SETLEGEND, (void *)spec);
    if (r != NEMA_OK) return r;
    if (c) {
        api_ext_t *x = api_ext(c);
        x->legend_fd = *spec ? fd : -1;
    }
    return 0;
}

void api_legend_release(api_ctx_t *c) {
    api_ext_t *x = api_ext(c);
    if (x->legend_fd < 0) return;
    nema_ioctl(x->legend_fd, NEMA_TIOC_SETLEGEND, (void *)"");
    x->legend_fd = -1;
}
