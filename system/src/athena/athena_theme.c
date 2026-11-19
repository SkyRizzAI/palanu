/* Athena's themes: the tokens every part of Athena draws with (colours, corner radii, title alignment, button case, shadows) and its
 * layout (where things go). A theme is more than a palette: Cupertino has centred titles, round corners and soft shadows; Material is
 * flat, with small radii, round icons, left-aligned titles and upper-case buttons; Softkey is the feature-phone way: a softkey bar at
 * the bottom of every screen (Options, the Enter action, Back), an idle home that opens a menu grid of the apps, titles on a pane,
 * a gradient highlight bar and the signal and battery as rising bars; Trackball is the older keyboard-phone way: no softkeys, a dark title
 * bar, an idle home with the clock and a row of apps along the bottom, the menu key (Tab) opening a grid of icons on the wallpaper with
 * the selected app's name as the title, and Options popping up at the right edge. The choice is the athena.theme setting, which only Athena's
 * session shows. */
#include <string.h>

#include "athena_int.h"
#include "pal/settings.h"

static const ath_theme_t THEMES[] = {
    {.id = "cupertino", .name = "Cupertino",
     .wall_top = 0x1A1750, .wall_bot = 0x7A3C8C, .surface = 0xF2F2F7, .row = 0xFFFFFF, .accent = 0x0A84FF, .ink = 0x1C1C1E,
     .muted = 0x8E8E93, .on = 0x34C759, .danger = 0xFF3B30, .track = 0xE5E5EA, .key_mod = 0xD1D1D6, .bar_bg = 0x000000,
     .r_card = 8, .r_row = 6, .r_icon = 10, .title_left = false, .caps_buttons = false, .flat = false,
     .term_bg = 0x1C1C1E, .term_fg = 0xF2F2F7},
    {.id = "material", .name = "Material",
     .wall_top = 0x263238, .wall_bot = 0x00695C, .surface = 0xFAFAFA, .row = 0xFFFFFF, .accent = 0x009688, .ink = 0x212121,
     .muted = 0x757575, .on = 0x009688, .danger = 0xD32F2F, .track = 0xBDBDBD, .key_mod = 0xCFD8DC, .bar_bg = 0x00796B,
     .r_card = 4, .r_row = 2, .r_icon = LV_RADIUS_CIRCLE, .title_left = true, .caps_buttons = true, .flat = true,
     .term_bg = 0x263238, .term_fg = 0xECEFF1},
    {.id = "softkey", .name = "Softkey",
     .wall_top = 0x06307A, .wall_bot = 0x5FA6E8, .surface = 0xFFFFFF, .row = 0xFFFFFF, .accent = 0x1A5FC4, .ink = 0x12161C,
     .muted = 0x5B6573, .on = 0x2E9E3E, .danger = 0xC62828, .track = 0xB8C4D0, .key_mod = 0xC5D2E2, .bar_bg = 0x0A2A66,
     .r_card = 4, .r_row = 3, .r_icon = 6, .title_left = true, .caps_buttons = false, .flat = true,
     .hl_top = 0x5AA4F2, .hl_bot = 0x1A56B8, .pane_top = 0x3474D4, .pane_bot = 0x0B3784,
     .softkeys = true, .idle_home = true, .title_pane = true, .level_bars = true, .term_bg = 0x000000, .term_fg = 0xC6F5C0},
    {.id = "trackball", .name = "Trackball",
     .wall_top = 0x0C1018, .wall_bot = 0x34425A, .surface = 0xFFFFFF, .row = 0xFFFFFF, .accent = 0x2A6FD6, .ink = 0x111418,
     .muted = 0x5F6670, .on = 0x3D9A3D, .danger = 0xC0392B, .track = 0xB9C0C8, .key_mod = 0xC9CED6, .bar_bg = 0x16191E,
     .r_card = 3, .r_row = 3, .r_icon = 5, .title_left = true, .caps_buttons = false, .flat = true,
     .hl_top = 0x78B6F7, .hl_bot = 0x1C58B8, .pane_top = 0x5C626B, .pane_bot = 0x1E2125,
     .idle_home = true, .title_pane = true, .level_bars = true, .dock = 5, .menu_cols = 5, .menu_dark = true, .popup_right = true,
     .term_bg = 0x0B0D10, .term_fg = 0xE8E8E8},
};

const ath_theme_t *ath_T = &THEMES[0];


/* The theme the setting names; the first one when it names none. */
const ath_theme_t *ath_theme_wanted(void) {
    char id[16] = "";
    psys_settings_get("athena.theme", id, sizeof(id));
    for (size_t i = 0; i < sizeof(THEMES) / sizeof(THEMES[0]); i++)
        if (!strcmp(THEMES[i].id, id)) return &THEMES[i];
    return &THEMES[0];
}
