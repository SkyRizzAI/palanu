/* The input menu: what Fn tapped twice opens (it used to open the on-screen keyboard at once). A small modal over the screen in
 * front with the things a 39-key keyboard needs a shortcut for: the on-screen keyboard, the symbol layer locked on, the digits
 * locked on, Caps Lock, and Ctrl for the next key. The focus starts on the keyboard. Up and Down move, and so does one tap of Fn
 * (it is Down); Enter picks, and the letter printed in each row picks at once. Fn tapped twice closes the menu, as Esc does, so
 * the same gesture opens and closes it. Esc closes. The modes are pal_input_mode_t (pal/input.h); a small mark in the status bar shows the
 * ones that are on. */
#ifndef PAL_INPUT_MENU_H
#define PAL_INPUT_MENU_H

#include "nema/types.h"

/* Opens the menu over the screen in front (any task). */
nema_err_t pal_input_menu_open(void);

/* The Fn double tap (PAL_KEY_VKB), desktop task only: closes the on-screen keyboard if it is open, closes the menu if it is open,
 * else opens it. */
void pal_input_menu_fn_double(void);

#endif
