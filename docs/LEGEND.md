# Legend bar (key legend)

The legend bar is a strip at the bottom of the screen that shows what the keys do on the screen currently in front, for example
`ENTER open`, `ESC back`. It is the sibling of the status bar: drawn by the desktop (compositor), owned by the system, and it takes
its height from the content area. Screens no longer draw their own footer.

**For now, only on the home screen.** At the owner's request, the bar is only drawn when the view that owns it is the home screen (`desktop.c`,
`legend_owner`): Settings and other screens are cleaner without the bar, and their content area is no longer reduced by the bar's height. Every view still
declares its legend and the app API does not change; `pal_legend_home_only(false)` shows it on all screens again (used by tests).
On home: `[↵] Apps` and `[Fn+W] Control`.

Code: `system/include/pal/legend.h`, `system/src/legend.c` (layout and drawing), `system/src/kui/kui_legend.c` (a single
item), `system/src/desktop.c` (bar owner, area, `pal_legend_set`), `boards/hhv2/hhv2_legend.c` (the hhv2 key table).
Tests: `tests/unit/test_legend.c`, `tests/unit/test_tty_edit.c` (legend text in the TTY). App API: `api/palanu/legend.h`,
`system/api/legend.c`.

## Principles

1. **Actions, not keys.** A view declares `PAL_ACT_OK`, `PAL_ACT_BACK`, `PAL_ACT_MOVE_V`, and so on. The board maps
   each action to its own key cap text. So one declaration works on every board.
2. **Actions without a key are hidden.** A board that has no key for an action (hhv2 has no menu key) does not
   draw that item. Items are never stacked.
3. **The bar belongs to the system.** Views do not draw the bar and do not reserve space for it. The desktop shrinks the area of every
   layer above the bar.
4. **Per view.** Every `pal_view_t` has `legends` and `nlegends`. Static values are declared in the view's table; values that
   change with state are set with `pal_legend_set`.

## API

```c
typedef enum {
    PAL_ACT_NONE, PAL_ACT_OK, PAL_ACT_BACK, PAL_ACT_MOVE, PAL_ACT_MOVE_H, PAL_ACT_MOVE_V,
    PAL_ACT_UP, PAL_ACT_DOWN, PAL_ACT_TAB, PAL_ACT_MENU, PAL_ACT_DELETE, PAL_ACT_HOME,
    PAL_ACT_CUSTOM, PAL_ACT_COUNT
} pal_legend_action_t;

typedef struct {
    pal_legend_action_t action;
    const char *label;    /* "open", "back"; NULL or "" shows only the key */
    const char *key_text; /* PAL_ACT_CUSTOM: the key. Other actions: replaces the board key. NULL: board key */
} pal_legend_item_t;

void pal_legend_set(const pal_legend_item_t *items, int n); /* legend of the view the desktop is currently running */
void pal_legend_clear(void);
```

Layers that have a legend:

- `pal_view_t.legends` + `.nlegends`: a static array, which must stay alive while the view is in use.
- `PAL_VIEW_NOLEGEND`: no bar while this view is on top (or while this view is the layer that would be used by the bar).

Bar owner rules (`legend_owner` in `desktop.c`):

- From the topmost overlay downward. An overlay that has its own legend (or an overlay that does not cover, such as a dialog
  without a legend) gives way to the layer below it. An opaque overlay without a legend, or the `NOLEGEND` flag, hides the bar.
- The front view is the last fallback.
- The `ui.legend` setting (`0` or `1`, default `1`) turns the bar off on all screens.

Area: `pal_desktop_area` cuts `PAL_LEGEND_H_PX` (22 physical pixels, 11 logical rows) from the bottom when the bar is shown.

`pal_legend_set` only works on the desktop task, and only for the view currently being run (draw, key, tick, pump, enter,
leave). The same array and count pair changes nothing. A change triggers a full repaint.

## hhv2 key table

| Action | Key |
| --- | --- |
| `PAL_ACT_OK` | the Enter symbol (return arrow, `KUI_LEGEND_ENTER`, drawn as an icon) |
| `PAL_ACT_BACK` | `ESC` |
| `PAL_ACT_MOVE` | `Fn+WASD` |
| `PAL_ACT_MOVE_H` | `Fn+A/D` |
| `PAL_ACT_MOVE_V` | `Fn+W/S` |
| `PAL_ACT_UP` | `Fn+W` |
| `PAL_ACT_DOWN` | `Fn+S` |
| `PAL_ACT_TAB` | `TAB` |
| `PAL_ACT_DELETE` | `DEL` (the physical "del" key) |
| `PAL_ACT_HOME` | `Fn+Q` |
| `PAL_ACT_MENU` | (none, the item is hidden) |
| `PAL_ACT_CUSTOM` | (from `key_text`) |

Source: `boards/hhv2/hhv2_legend.c`, checked against `docs/KEYMAP.md`. A new board adds a `legend_keys` table to its
descriptor (`pal_board_desc_t`; `boards/_template` has no example yet).

## Layout

- One item: left.
- Two items: left and right.
- Three items: left, center, right.
- More than three: packed from the left with 6 pixels of spacing.
- Items that do not fit are dropped from the right. Maximum 12 items (`PAL_LEGEND_MAX`).

Item: the key inside an inverted capsule (ink, paper text) in `FONT_TINY` (so `Fn+W` fits in the small capsule; Enter is an icon),
then the label in ink in `FONT_6X8`. The capsule is 7 rows, centered on the 8-row label, inside the 11-row bar.

## Custom apps

Native apps (views compiled into the system) call `pal_legend_set` or give their view a `legends` array.
Nothing needs to be drawn.

```c
static const pal_legend_item_t LEG[] = {{PAL_ACT_OK, "open", NULL}, {PAL_ACT_BACK, "close", NULL}};
static pal_view_t V = {.id = "myapp", .title = "My app", .draw = draw, .key = key, .legends = LEG, .nlegends = 2};
```

An app that gives no legend gets no bar.

### WebAssembly apps (`.papp`)

`.papp` apps run in the terminal, so their legend is shown while that app's terminal is in front. One function, no
permission (`api/palanu/legend.h`):

```c
#include "palanu.h"

pal_legend_show("ok=Jump;back=Quit;[SPACE]=Pause"); /* set */
pal_legend_show("");                               /* clear */
```

Text: items are separated by `;`, each one `NAME` or `NAME=label`.

- `NAME` is an action: `ok`, `back`, `move`, `move_h`, `move_v`, `up`, `down`, `tab`, `menu`, `delete`, `home` (upper or
  lower case does not matter). The board decides the key text, so `ok` shows as `ENTER` on hhv2.
- Other keys are written in square brackets: `[SPACE]=Pause`, `[Q]=Quit`. At most 15 characters.
- Without a label (`move_v`) only the key is shown.
- At most 12 items and 159 bytes. Invalid text is rejected with `PAL_ERR_INVAL` and the old legend stays.

The legend stays until the app replaces it or finishes; when the app finishes (normally, Ctrl-C, or kill) the system
clears it. The bar uses 22 pixels from the bottom of the screen, so setting or clearing a legend changes the number of terminal rows:
the app gets a `RESIZE` event as when the terminal size changes. An app without a terminal gets a negative error.

The path:

1. `psys_api_legend_show` (`system/api/legend.c`) validates the text with `pal_legend_parse`, then sends it to the app's terminal
   with the ioctl `NEMA_TIOC_SETLEGEND`. The kernel only stores the text and a change counter.
2. The terminal (`apps/terminal.c`, `legends_sync` in the pump) reads the text of the session in front with `nema_tty_get_legend`.
   If the counter changed, it parses the text and calls `pal_legend_set`, then adjusts the terminal size.
3. `api_legend_release` clears the text when the app finishes.

## What was replaced

| Before | Now |
| --- | --- |
| `pal_ui_footer`, `pal_ui_hint_t`, `pal_ui_footer_h` | `pal_legend_set` / `legends` |
| `theme.comp.footer`, `theme.comp.footer_height` | removed; the bar is not themed |
| `pal_footer_hints_for` (nav) | removed; `nav.c` only maps keys to intents |

Screens that were migrated: switcher, text input, notifications (list and detail), launcher (info page), animation (player),
pairing (`settings-pair`), and home.
