/* Mission Control's focus model (internal, pure): the grid of tiles and sliders, as the reference MissionControlScreen lays it out in
 * landscape. The view (ui_control.c) draws what this model says and runs the actions. */
#ifndef SYSTEM_UI_CONTROL_H
#define SYSTEM_UI_CONTROL_H

#include <stdbool.h>

typedef enum {
    CTL_DARK,
    CTL_WIFI,
    CTL_LOCK,
    CTL_SETTINGS,
    CTL_RESTART,
    CTL_BRIGHT, /* slider, in every row */
    CTL_VOL,    /* slider, in every row */
    CTL_COUNT,
} ctl_item_t;

/* The reference directions: Up and Down are Prev and Next, Left and Right are AdjustDown and AdjustUp. */
typedef enum { CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT } ctl_dir_t;

/* The item one step from focus in dir, on the 2 x 5 landscape grid. Left and Right skip empty cells; Up and Down keep the
 * column. On a slider Up and Down do not move (the view adjusts the value). The edges stay put. */
ctl_item_t pal_ui_ctl_move(ctl_item_t focus, ctl_dir_t dir);

/* One key press: brightness in percent, volume in percent (the reference steps: 16 of 255 and 5 of 100). */
#define CTL_BRIGHT_STEP 6
#define CTL_VOL_STEP 5

#endif
