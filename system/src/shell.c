/* Shell services as data (pal/shell.h): the service in front, described for the session or the console that draws it. */
#include <string.h>

#include "desktop_int.h"
#include "pal/shell.h"

bool pal_shell_describe(pal_view_t *v, pal_shell_view_t *out) {
    if (!v || !v->describe || !out) return false;
    memset(out, 0, sizeof(*out));
    return v->describe(v, out);
}

bool pal_shell_top(pal_shell_view_t *out) {
    pal_view_t *top = ovl_top();
    if (!top || !top->describe) return false;
    if (!out) return true;
    return pal_shell_describe(top, out);
}

bool pal_shell_key(const pal_key_t *k) {
    pal_view_t *top = ovl_top();
    if (!top || !top->describe) return false;
    bool handled = top->key && top->key(top, k);
    if (!handled && k->sym == PAL_KEY_ESC) ovl_remove_now(top); /* as the compositor does for an overlay */
    pal_desktop_invalidate();
    return true;
}
