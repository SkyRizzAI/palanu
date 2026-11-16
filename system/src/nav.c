/* PalanuOS navigation intents: the one table from keys to what they mean on screen. Every screen asks pal_nav_of, so a
 * key that moves a list means the same thing everywhere. The legend bar names the same intents as actions (pal/legend.h), and
 * the board maps those to key caps. Fn+W/A/S/D give the arrows (the board maps them in input.c); Enter and Esc and Tab are
 * plain keys. */
#include "pal/nav.h"

typedef struct {
    pal_nav_t intent;
    pal_keysym_t sym;
} nav_row_t;

static const nav_row_t ROWS[] = {
    {PAL_NAV_UP, PAL_KEY_UP},
    {PAL_NAV_DOWN, PAL_KEY_DOWN},
    {PAL_NAV_LEFT, PAL_KEY_LEFT},
    {PAL_NAV_RIGHT, PAL_KEY_RIGHT},
    {PAL_NAV_OK, PAL_KEY_ENTER},
    {PAL_NAV_BACK, PAL_KEY_ESC},
    {PAL_NAV_MENU, PAL_KEY_TAB},
};

pal_nav_t pal_nav_of(const pal_key_t *k) {
    if (!k) return PAL_NAV_NONE;
    for (unsigned i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++)
        if (ROWS[i].sym == k->sym) return ROWS[i].intent;
    return PAL_NAV_NONE;
}

pal_keysym_t pal_nav_key(pal_nav_t n) {
    for (unsigned i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++)
        if (ROWS[i].intent == n) return ROWS[i].sym;
    return PAL_KEY_NONE;
}
