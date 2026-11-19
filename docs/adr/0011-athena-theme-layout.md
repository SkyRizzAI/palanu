# ADR-0011: An Athena theme carries layout, not only tokens

Status: accepted (19 November 2026).

## Context
Athena's themes (Cupertino, Material) were colours and shapes over one structure: a status bar on top, a home grid of widgets and apps,
a page per form with Back on Esc. The owner asked for a third theme in the feature-phone style: a softkey bar at the bottom of every
screen (Options, the Enter action, Back), an idle home that opens a menu grid of the apps, titles on a pane, a gradient highlight bar,
and the signal and battery as rising bars. That is a different structure, not a palette.

Three ways were open:

1. A fourth desktop session. It would repeat Athena's lock, pages, shell services, widgets and theme rebuild, and apps would gain nothing
   that Athena does not already give them.
2. `if (theme == softkey)` in Athena's parts. Every later theme would add more branches.
3. Layout fields in the theme, read by Athena's parts as data.

## Decision
Option 3. `ath_theme_t` gains layout fields; zero is the existing behaviour, so Cupertino and Material are unchanged:

- `softkeys`: a 20-row bar at the bottom with three labels set by what is in front. Tab is the left softkey (Options, or Menu on the idle
  screen), Enter the middle action, Esc the right softkey (Back, Exit, Cancel). These are keys the board already has; an app in front
  keeps its own Tab and Esc.
- `idle_home`: the home is the widgets stacked across the screen (`rows` is a widget's share of the height); Enter or the left softkey
  opens the app menu, a title pane over a grid of three columns.
- `title_pane`: a form's title sits on a band in the pane colours.
- `level_bars`: the status bar shows the Wi-Fi signal and the battery as bars.
- `hl_top`, `hl_bot`, `pane_top`, `pane_bot`: the highlight and pane gradients.

The softkey bar also stays over apps, so the compositor needs to know its rows: `pal_desktop_chrome_t` gains `bottom_px` (appended; the
area of every layer stops above it) and the display gains `pal_display_reserve_bottom`. Athena sets one of two chrome tables by theme and
sets it again when the theme changes.

The theme is named `softkey`: the phone makers' names are trademarks and the repository only names its own things. Its icons and colours
are drawn fresh, nothing is copied.

## Consequences
- Forms (Settings, Monitor, apps at API level 6) take the new look without a change, since Athena lays out their rows.
- A later theme with another structure adds a field here, not a branch in the parts.
- The menu grid shows the launcher's apps in its order; it is not yet editable, as the grid home is not.
- Over a terminal app the softkey bar shows only Back: the terminal's own keys are its own.
