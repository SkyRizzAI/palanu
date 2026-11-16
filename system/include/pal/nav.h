/* PalanuOS navigation intents: one mapping from physical keys to what a key means on screen, so that every screen
 * asks the same function. The mapping is in system/src/nav.c; the legend bar names the same intents (pal/legend.h). */
#ifndef PAL_NAV_H
#define PAL_NAV_H

#include "pal/input.h"

typedef enum {
    PAL_NAV_NONE = 0,
    PAL_NAV_UP,    /* Fn+W */
    PAL_NAV_DOWN,  /* Fn+S */
    PAL_NAV_LEFT,  /* Fn+A */
    PAL_NAV_RIGHT, /* Fn+D */
    PAL_NAV_OK,    /* Enter */
    PAL_NAV_BACK,  /* Esc */
    PAL_NAV_MENU,  /* Tab */
    PAL_NAV_COUNT,
} pal_nav_t;

/* The intent of a key event, or PAL_NAV_NONE. Repeats count the same as presses. */
pal_nav_t pal_nav_of(const pal_key_t *k);
/* The key that produces an intent (the primary one). PAL_KEY_NONE for PAL_NAV_NONE. */
pal_keysym_t pal_nav_key(pal_nav_t n);

#endif
