/* Mission Control's focus grid (pure; see ui_control.h). The reference landscape layout: row 0 Dark, Wi-Fi, Lock; row 1 Settings,
 * Restart and an empty cell; the brightness and volume sliders in the last two columns of both rows. */
#include "ui_control.h"

#define EMPTY CTL_COUNT

static const ctl_item_t GRID[2][5] = {
    {CTL_DARK, CTL_WIFI, CTL_LOCK, CTL_BRIGHT, CTL_VOL},
    {CTL_SETTINGS, CTL_RESTART, EMPTY, CTL_BRIGHT, CTL_VOL},
};

static void find(ctl_item_t it, int *r, int *c) {
    *r = *c = 0;
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 5; k++)
            if (GRID[i][k] == it) {
                *r = i;
                *c = k;
                return;
            }
}

ctl_item_t pal_ui_ctl_move(ctl_item_t focus, ctl_dir_t dir) {
    int r, c;
    find(focus, &r, &c);
    bool slider = focus == CTL_BRIGHT || focus == CTL_VOL;
    if (slider && (dir == CTL_UP || dir == CTL_DOWN)) return focus;
    if (dir == CTL_LEFT || dir == CTL_RIGHT) {
        int step = dir == CTL_LEFT ? -1 : 1;
        for (int k = c + step; k >= 0 && k < 5; k += step)
            if (GRID[r][k] != EMPTY) return GRID[r][k];
        return focus;
    }
    int nr = r;
    if (dir == CTL_UP && r > 0) nr = r - 1;
    if (dir == CTL_DOWN && r < 1) nr = r + 1;
    ctl_item_t next = GRID[nr][c];
    return next == EMPTY ? focus : next;
}
