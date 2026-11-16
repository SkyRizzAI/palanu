/* The legend bar's key caps on hhv2 (pal/legend.h): the key each semantic action is printed as. The keys are the ones of
 * hhv2_keyboard.c and docs/KEYMAP.md: Fn with W A S D is the arrows, Fn with Q is Home, and the physical "del" key is DEL. The
 * board has no menu key, so PAL_ACT_MENU is absent and its legend is hidden. */
#include "pal/kui.h"
#include "pal/legend.h"

const char *const HHV2_LEGEND_KEYS[PAL_ACT_COUNT] = {
    [PAL_ACT_OK] = KUI_LEGEND_ENTER, /* the Enter symbol: the cap stays small */
    [PAL_ACT_BACK] = "ESC",
    [PAL_ACT_MOVE] = "Fn+WASD",
    [PAL_ACT_MOVE_H] = "Fn+A/D",
    [PAL_ACT_MOVE_V] = "Fn+W/S",
    [PAL_ACT_UP] = "Fn+W",
    [PAL_ACT_DOWN] = "Fn+S",
    [PAL_ACT_TAB] = "TAB",
    [PAL_ACT_DELETE] = "DEL",
    [PAL_ACT_HOME] = "Fn+Q",
};
