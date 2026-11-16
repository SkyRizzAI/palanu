/* Key legends (API level 2): the bar at the bottom of the screen that says what the keys do. While the app's terminal is in
 * front, the bar shows the legends the app published; the system's own apps fill the bar themselves. A legend names an
 * action, not a key: the board prints its own key cap, so one text works on every board ("ok" is ENTER on hhv2). The text
 * is items separated by ';', each NAME or NAME=label. NAME is one of ok, back, move, move_h, move_v, up, down, tab, menu,
 * delete, home (any case), or a key cap in brackets for any other key: "ok=Jump;back=Quit;[SPACE]=Pause". An action the
 * board has no key for is not shown, and items that do not fit the width are dropped from the right. The legends stay until
 * the app changes them or ends. Showing or hiding the bar changes the terminal's rows: the app gets a RESIZE event. No
 * permission is needed. */
#ifndef PALANU_API_LEGEND_H
#define PALANU_API_LEGEND_H

#include "annot.h"

PAL_SINCE(2) PAL_WASM(legend_show)
PAL_DOC("Shows the legends in spec (NUL-terminated, up to 159 bytes, at most 12 items) while the app's terminal is in front; an empty spec removes them. Returns 0, PAL_ERR_INVAL (malformed text, unknown name, cap over 15 characters), or a negative error when the app has no terminal.")
int32_t pal_legend_show(const char *spec);

#endif
