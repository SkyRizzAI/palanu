/* PalanuOS legend bar: the strip at the bottom of the screen that says what the keys do on the screen in front. It is the
 * counterpart of the status bar (pal/desktop.h): the desktop draws it after the layers, and it takes its rows out of the
 * content area, so a view never draws it and never reserves room for it.
 *
 * A legend names an action, not a key: "OK", "BACK", "MOVE_V". The board says which key cap each action prints (the table
 * pal_board_desc_t.legend_keys, boards/hhv2/hhv2_legend.c for hhv2). An action the board has no key for is hidden, so one
 * view declares its legends once and every board shows the keys it really has. An app-specific key is PAL_ACT_CUSTOM with
 * key_text. docs/LEGEND.md has the design, the board table and how an app uses it. */
#ifndef PAL_LEGEND_H
#define PAL_LEGEND_H

#include <stddef.h>

#include "pal/gfx.h"
#include "pal/kui.h"

/* The bar's height in physical pixels at the current UI scale (22 at 2x): the bottom rows of the screen. */
int pal_legend_h_px(void);
#define PAL_LEGEND_MAX 12                  /* items drawn at most; the rest are never laid out */

typedef enum {
    PAL_ACT_NONE = 0, /* not an action: the item is hidden */
    PAL_ACT_OK,       /* confirm, open, press: Enter on hhv2 */
    PAL_ACT_BACK,     /* back, close, cancel: Esc on hhv2 */
    PAL_ACT_MOVE,     /* the four arrows together */
    PAL_ACT_MOVE_H,   /* left and right */
    PAL_ACT_MOVE_V,   /* up and down */
    PAL_ACT_UP,       /* up alone (a view's own binding, such as the home screen's Control) */
    PAL_ACT_DOWN,     /* down alone */
    PAL_ACT_TAB,      /* the Tab key, as a command (clear all, the info page) */
    PAL_ACT_MENU,     /* a menu key, where the board has one */
    PAL_ACT_DELETE,   /* delete the item in focus */
    PAL_ACT_HOME,     /* go to the home screen */
    PAL_ACT_CUSTOM,   /* any other key: key_text is the cap */
    PAL_ACT_COUNT
} pal_legend_action_t;

typedef struct {
    pal_legend_action_t action;
    const char *label;    /* what it does: "open", "back". NULL or "" shows the cap alone */
    const char *key_text; /* PAL_ACT_CUSTOM: the cap. Other actions: a cap that replaces the board's. NULL: the board's */
} pal_legend_item_t;

/* The key cap the running board prints for an action, or NULL when the board has no key for it (the item is then hidden). */
const char *pal_legend_board_key(pal_legend_action_t a);

/* Layout (pure, logical pixels of a bar width wide). widths[i] is item i's full width (see kui_legend_item_w). Writes the x of
 * each placed item into xs and returns how many were placed; the items that do not fit are dropped from the right, never
 * overlapped. One item sits left, two left and right, three left, centre and right; more are packed left with gaps. */
int pal_legend_layout(const int *widths, int n, int width, int *xs);

/* Parses the text form an app publishes (pal_legend_show in api/palanu/legend.h): items separated by ';', each NAME or
 * NAME=label, where NAME is an action (ok, back, move, move_h, move_v, up, down, tab, menu, delete, home; any case) or a
 * key cap in brackets ("[SPACE]=jump" is PAL_ACT_CUSTOM). Spaces around names and labels are dropped; empty items are
 * skipped. The text is copied into buf (cap bytes) and the items point into it. Returns the item count (0 for an empty
 * text), or -1 when an item is malformed, a name is unknown, a cap is empty or longer than 15, there are more than max
 * items, or buf is too small. */
int pal_legend_parse(const char *spec, pal_legend_item_t *items, int max, char *buf, size_t cap);

/* Draws items in the bar at the bottom of c (the last pal_legend_h_px() rows). Items whose key is hidden are skipped. Called by
 * the desktop; a view never calls it. */
void pal_legend_draw(pal_canvas_t *c, const pal_legend_item_t *items, int n);

#endif
