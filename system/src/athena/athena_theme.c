/* Athena's themes: the tokens every part of Athena draws with (colours, corner radii, title alignment, button case, shadows). A theme
 * is more than a palette: Cupertino has centred titles, round corners and soft shadows; Material is flat, with small radii, round icons,
 * left-aligned titles and upper-case buttons. The choice is the athena.theme setting, which only Athena's session shows. */
#include <string.h>

#include "athena_int.h"
#include "pal/settings.h"

static const ath_theme_t THEMES[] = {
    {.id = "cupertino", .name = "Cupertino",
     .wall_top = 0x1A1750, .wall_bot = 0x7A3C8C, .surface = 0xF2F2F7, .row = 0xFFFFFF, .accent = 0x0A84FF, .ink = 0x1C1C1E,
     .muted = 0x8E8E93, .on = 0x34C759, .danger = 0xFF3B30, .track = 0xE5E5EA, .key_mod = 0xD1D1D6, .bar_bg = 0x000000,
     .r_card = 14, .r_row = 8, .r_icon = 10, .title_left = false, .caps_buttons = false, .flat = false},
    {.id = "material", .name = "Material",
     .wall_top = 0x263238, .wall_bot = 0x00695C, .surface = 0xFAFAFA, .row = 0xFFFFFF, .accent = 0x009688, .ink = 0x212121,
     .muted = 0x757575, .on = 0x009688, .danger = 0xD32F2F, .track = 0xBDBDBD, .key_mod = 0xCFD8DC, .bar_bg = 0x00796B,
     .r_card = 4, .r_row = 2, .r_icon = LV_RADIUS_CIRCLE, .title_left = true, .caps_buttons = true, .flat = true},
};

const ath_theme_t *ath_T = &THEMES[0];

const char *const ATH_THEME_IDS[] = {"cupertino", "material", NULL};

/* The theme the setting names; the first one when it names none. */
const ath_theme_t *ath_theme_wanted(void) {
    char id[16] = "";
    psys_settings_get("athena.theme", id, sizeof(id));
    for (size_t i = 0; i < sizeof(THEMES) / sizeof(THEMES[0]); i++)
        if (!strcmp(THEMES[i].id, id)) return &THEMES[i];
    return &THEMES[0];
}
