# ADR-0009: Desktop environments own the chrome, and one of them is drawn with LVGL in full colour

Status: accepted.

## Context
The compositor had a replaceable home view, a launcher with four skins and themes with replaceable components, but the status bar, the
global keys (Fn twice, the cycle key) and their menus were wired into the compositor for one look. The owner wants the Linux model: the base
system is a shell on a TTY, and a desktop environment provides everything graphical around the apps, with its own bar, keys and menus.
We also want a full-colour desktop built with LVGL, the widely used embedded GUI library, and a rule for how apps look under each desktop.

## Decision
- A registry of desktop environments (`pal/desktops.h`): id, start, optional stop; one is active; `desktop.env` keeps the choice; the shell
  command `desktop` lists and switches. `classic` is the fallback and the only one safe mode starts.
- A desktop owns its chrome (`pal_desktop_chrome_t` in `pal/desktop.h`): whether the compositor draws its bar and legend bar, rows kept for the
  desktop's own bar, a first look at every key, and whether it presents the whole screen itself. No chrome is the classic behaviour, so every
  existing screen and golden image is unchanged.
- Three desktops: `classic` (the current look), `athena` (LVGL, own bar, own Keyboard and Recent apps sheets) and `console` (no desktop: the
  Terminal on the whole screen, no global keys).
- Full colour: the panel driver may take RGB565 images (`present_rgb565`, appended to `pal_display_ops_t`), and a desktop sends them with
  `pal_display_present_rgb565`. Athena renders with LVGL into one PSRAM frame and sends it directly, so it is not limited to the 8-bit palette.
  The 8-bit framebuffer stays the surface for views and overlays.
- Apps: TTY apps look the same everywhere (the desktop frames the Terminal); toolkit apps (`pal/ui.h`, the theme components) follow the
  active theme, which a desktop may select, and a menu whose rows are data is bound to its view (`pal_menu_bind`) so a desktop can lay it out
  itself (Athena draws Settings as a full-colour grouped list); canvas apps draw raw pixels and look the same everywhere. A full-colour app surface is a later API
  addition with its own level.
- LVGL 9.2.2 (MIT) is vendored unmodified; its pool, frame and state live in PSRAM.

## Consequences
- A new desktop is one file and one `pal_desktops_register` call. The compositor gained a small hook table, not a second code path.
- The firmware image grew by about 400 KB to 91% of the OTA slot: over the 85% release warning, under the 95% failure. Trimming LVGL further
  or a larger OTA slot is needed before more features.
- A desktop that reads the status (an ADC sample) too often trips the interrupt watchdog on hhv2; the rule is once a second.
- Under Athena, screens that are not bound menus (Files, Logs, dialogs) keep the classic drawing until their toolkits are data a desktop can lay out.
