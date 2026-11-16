/* Layout math shared by the screens. Pure: no canvas, no font, so the host tests cover the edges. The launcher skins and the
 * list rows lay themselves out in kui_launcher.c and kui_list.c (the reference arithmetic), so only the window helper is here. */
#include "pal/ui.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int pal_ui_window_first(int n, int cursor, int visible) {
    if (visible < 1) visible = 1;
    if (n <= visible) return 0;
    cursor = clampi(cursor, 0, n - 1);
    return clampi(cursor - visible / 2, 0, n - visible);
}
