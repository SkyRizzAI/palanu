# PalanuOS user interface (UI)

This document describes the screens PalanuOS draws (T5.7 to T5.16): what each screen is, the settings that affect it,
how to add a launcher skin or wallpaper, how to make icons, and the transition point to the desktop contract.
The style: a status bar on top, pill-shaped key legends at the bottom, pixel-art tiles, and theme palettes.
All icon assets are original drawings.

## Files

| File | Content |
| --- | --- |
| `assets/icons/<name>@<size>.txt` | Pixel-art icon source: `#` ink, `+` accent, `.` empty. Sizes 32, 16, and 8. |
| `forge/tools/icons.ts` | Turns the sources into `system/src/icons_data.c` and `system/include/pal/icons_data.h`. `--check` is run by `./pal check`. |
| `system/include/pal/icons.h`, `system/src/icons.c` | `pal_icon_find`, `pal_icon_blit`, `pal_icon_draw` (bitmap first, then the old vector shapes, then `default`). |
| `system/include/pal/ui.h` | The API shared by the screens: settings, formatting, layout, PIN, drawing helpers, and screens. |
| `system/src/ui_prefs.c` | Screen settings in `/data/ui` (`launcher`, `wallpaper`, `pin` as a SHA-256 hash). |
| `system/src/ui_fmt.c`, `ui_layout.c`, `ui_pin.c`, `ui_control_model.c` | Pure parts (no canvas): clock text and notification age, layout, the PIN engine, Control focus. |
| `system/src/ui_common.c` | Drawing helpers: pill footer, chip, vertical slider, scrollbar, enlarged text. |
| `system/src/launcher.c`, `ui_launcher.c`, `ui_launcher.h`, `launcher_fixture.c`, `kui/kui_launcher.c` | The launcher (view `launcher`) and its four skins: strip, grid, compact, list. |
| `system/src/ui_home.c`, `ui_wall.c`, `ui_wall.h` | The home screen and live wallpaper. |
| `system/src/ui_lock.c` | The lock screen. |
| `system/src/ui_control.c` | Control center. |
| `system/src/ui_notify.c` | The notification center and detail page. |
| `system/src/ui_gallery.c` | Icon contact sheet (dev, `uishow gallery`). |
| `system/src/ui_wire.c` | The only place that calls the desktop API for these screens (see "Transition"). |
| `apps/cmd_uishow.c` | The `uishow` command: opens a screen in a fixed state (for golden images). |
| `tests/unit/test_ui.c` | Host tests for the pure parts and for the wallpaper. |

## Screens

**Home** (`home`, its own screen, no system status bar). The wallpaper fills the screen; a large pixel clock `HH:MM`
(8x16 font enlarged 4 times) and the date below it; a chip strip on top (Wi-Fi, BLE, Link, battery, a bell with the
unread count); the legend at the bottom through the system legend bar (`docs/LEGEND.md`): `ENTER Launcher`, `UP Control`, `DOWN Notifications`.
The wallpaper is redrawn only in the parts that changed, with a 200 ms tick (5 fps). While home is covered by an overlay
(`obscured`) the animation stops and resumes on `exposed`.

**Launcher** (`launcher`, status bar shown). Four skins: `strip`, `grid`, `compact`, `list` (drawn through kui,
`system/src/kui/kui_launcher.c`). The app list comes from the registry (`psys_reg_get`, hidden entries
are not shown); only pinned entries (`pal_launcher_pinned`: Terminal, Apps, Files, Settings, Logs) are shown. Other apps are in **Apps** (`applist`, `system/src/applist.c`, the list screen): grouped by category (folder), with a Back row inside a folder; ESC goes up one folder and then back to the launcher. The icon is taken from `icon` in the manifest, and if
empty from the id. Left/up and right/down move focus by one entry. `TAB` opens the info page (name, id, version,
kind, source, memory, caps). `ESC` goes back to home. There is no footer.

**Lock** (`lock`, full screen, opaque). Clock and date, a padlock icon with the text `LOCKED`, and a summary of the two
newest notifications. Without `security.pin`: `ENTER` twice unlocks. With a PIN: a number pad (type digits,
`BKSP` deletes, `ENTER` submits, `ESC` clears); a wrong PIN shows "Wrong PIN".
Unlocking uses `pal_desktop_unlock()` through `pal_ui_unlock()`.

**Lock with animation.** With `ui.lock_anim` (default `sleepy-cat`), the animation at 1x is to the left of the clock, and LOCKED, the hint, and the
notification summary move down. The animation stops when the screen sleeps or when an overlay covers it. `none` uses the plain layout.
See `docs/ANIM.md`.

**Splash** (`splash`, full screen, no status bar). Shown at boot: the PalanuOS mark with a moon orbiting the ring, the name,
a progress bar from the boot stages (`pal_boot_mark`) and the name of the last stage. It ends after all stages are recorded and at least
1.5 seconds, then opens home. Keys are ignored during the splash. Restart uses the same splash with the text "Restarting".

**Control** (`control`, full screen, opaque). A grid of 3 rows x 2 columns of square tiles: Wi-Fi, Bluetooth, Dark, Lock,
Settings, Restart, each with an icon and a status line (On, Off, Starting, SSID, Now, Reboot). Two vertical sliders
on the right: brightness (0 to 100, immediate) and volume (placeholder, disabled until there is audio). Focus moves
with the arrows; `ENTER` runs a tile; Up and Down on a slider change its value (`Fn+W` and `Fn+S` produce
Up and Down); `+` and `-` also change brightness. Wi-Fi and Bluetooth are turned on and off through
`psys_svc_start` and `psys_svc_stop` (`radio-wifi`, `radio-ble`), like the `wifi` and `ble` commands. Dark and
brightness go through the settings API (stored). Restart asks for confirmation with `pal_dialog_confirm`.

**Notifications** (`notifications`, full screen, opaque). The header holds the count. The list is newest on top: the app icon
(based on the source id, or a bell), a bold title, one line of text (scrolling if focused and long), the age (`now`,
`12m`, `10:04`, `2d`), and an unread dot. `ENTER` opens the detail (wrapped text, Dismiss and Close buttons).
`TAB` clears all after confirmation. Leaving the screen marks everything as read. When empty it shows
a bell icon and "No notifications".

**Gallery** (`gallery`, hidden). A contact sheet of all icons at three sizes, for reviewing the pixel-art drawings.

## Settings

| Key | Values | Default | Notes |
| --- | --- | --- | --- |
| `ui.launcher` | `strip`, `grid`, `compact`, `list` | `strip` | Launcher skin. Takes effect immediately. |
| `ui.wallpaper` | `none`, `stars`, `city`, `waves`, `anim:NAME` | `stars` | Home wallpaper. `anim:NAME` plays an animation (`docs/ANIM.md`). Takes effect immediately. |
| `ui.wall_fit` | `center`, `fit`, `stretch`, `crop` | `fit` | How the wallpaper animation is filled into the screen. |
| `ui.wall_anchor` | nine anchors (`top-left` ... `bottom-right`) | `bottom` | Placement of the wallpaper animation. The clock plate stays in place. |
| `ui.lock_anim` | `none` or an animation NAME | `sleepy-cat` | The animation on the lock screen. |
| `security.pin` | 4 to 8 digits, or `""` to clear | empty | Stored as a hash. Read back as `set` or `none`. |
| `device.name` | device name | `PalanuOS` | Title of the strip skin. |
| `ui.dark`, `ui.theme`, `ui.size` | as before | | Used by all screens through the palette. |
| `ui.statusbar` | `0`, `1` | `1` | The status bar on top. Read by the desktop on every draw. |
| `ui.legend` | `0`, `1` | `1` | The legend bar at the bottom (`docs/LEGEND.md`). Read by the desktop on every draw. |
| `ui.clock24` | `0`, `1` | `1` | 24 hours (the current default). The clock renderer does not read it yet; `0` will mean 12 hours once wired. |
| `perf.overlay` | `0`, `1` | `0` | Performance overlay (runtime, not stored). |
| `notify.dnd` | `0`, `1` | `0` | No toasts and no sound. The notification list is still filled. |
| `notify.beep` | `0`, `1` | `1` | Buzzer sound on a notification (boards that have a buzzer). |
| `input.repeat_delay` | 120 to 1000 (ms) | `420` | Delay before a held key repeats. |
| `input.repeat_rate` | 20 to 200 (ms) | `55` | Delay between repeats. |
| `power.profile` | `performance`, `balanced`, `saver` | `balanced` | Only stored; there is no power service yet. |
| `log.level` | `error`, `warn`, `info`, `debug` | `info` | Console log level. Takes effect immediately. |
| `boot.target` | `desktop`, `console` | `desktop` | The screen in front at boot. |
| `dev.mode` | `0`, `1` | `1` | Developer mode (unsigned apps). |
| `profile.user` | text up to 23 characters | empty | User name. |
| `link.uart.trust` | `0`, `1` | `1` | The cable is trusted without a token. Same as `link trust uart`. |

### The Settings app (view `settings`)

Pages are built from `system/include/pal/menu.h` (the menu framework): a title, rows of type SECTION, SUBMENU, TOGGLE,
CHOICE, SLIDER, ACTION, INFO, and TEXT. Rows are data. The page stack uses Esc to go back, and the selection is
remembered per page. The main page groups: Display, Appearance, Controls, Wi-Fi, Bluetooth, Remote
(Palanu Link), Notifications, Sound & LED, Battery & power, Storage, Apps, Date & time, Profile & security, Updates,
Developer, About. Every value change goes through `psys_settings_set`. Blocking work (turning on a radio, scanning,
connecting Wi-Fi, removing an app) runs on a worker task (`apps/settings_job.c`), with a busy overlay.

To open a specific page: `uishow settings NAME` (e.g. `uishow settings wifi-scan`, `uishow settings apps`). The test
`tests/sim/settings_walk.sh` opens all pages in the simulator (`sim_settings_walk` in ctest). Golden: `settings-*`.

Values are changed through the settings API (`psys_settings_set`, used by the Settings app and the Control tiles). The values
`ui.launcher`, `ui.wallpaper`, and `security.pin` are stored in `/data/ui`.

## Adding a launcher skin

1. Write `kui_skin_<name>(kui_canvas_t *c, const char *title, const kui_item_t *it, int n, int cursor)` in
   `system/src/kui/kui_launcher.c` and declare it in `system/include/pal/kui.h`. A skin only draws
   within the canvas clip and does not change state.
2. Add a case in `ui_skin_draw` (`system/src/ui_launcher.c`) and a `PAL_UI_LAUNCHER_*` constant in `pal/ui.h`.
3. Add the name to `LAUNCHER_NAMES` (`ui_prefs.c`), to `LAUNCHER_CHOICES` (`settings_api.c`), and to `LAUNCH_TEXTS` (`settings_display.c`).
4. Pixel test: add a launcher scenario to `forge/tools/golden-scenes.json` and record its golden image (`bun forge/tools/golden.ts update launcher`).

## Adding a wallpaper

1. Add a `PAL_UI_WALL_*` constant in `pal/ui.h` and its name in `WALL_NAMES` (`ui_prefs.c`).
2. Write a function that draws inside rectangle `r` for frame `f` in `ui_wall.c`, then branch to it in `ui_wall_draw`.
   A wallpaper must be a pure function of (frame, rectangle): a partial repaint must equal the full drawing (test
   `wallpaper_is_deterministic_and_repaints_by_part`).
3. In `ui_home.c` `tick()`, decide which part changed and repaint only that part.
4. Add a choice in `WALL_CHOICES` (`settings_api.c`) and a golden scene `home-<name>`.

## Making icons

- One file per icon and size: `assets/icons/terminal@32.txt`. Size 8, 16, or 32; rows and columns equal.
- `#` ink (the tile's `ink` color), `+` accent (`PAL_C_ACCENT`), `.` empty.
- Draw 32-pixel icons with 2-pixel lines. 16-pixel icons are made from the 32 (2x2 blocks) and then checked; 8-pixel icons
  are drawn by hand. The 16-pixel battery icon is still drawn by hand.
- Run `bun forge/tools/icons.ts`, then `./pal check` (checks that the C file matches the sources).
- Review: `uishow gallery` or the golden `gallery`.
- An icon for an app: fill in `icon = "name"` in `apps/apps.toml`, then run `bun forge/packages/appgen/src/cli.ts`.

## Tests and golden images

- `./pal test`: `test_ui` checks the icon table, list window clipping, clock and notification age formats, the PIN engine, Control tile focus and text, and wallpaper repaint.
- `uishow <screen> [arguments]` opens a screen in a fixed state (clock 10:09, sample notifications, frozen wallpaper frame):
  `uishow home stars 12`, `uishow launcher grid`, `uishow lock`, `uishow control`, `uishow notifications [empty]`,
  `uishow gallery`. The screen is opened through the desktop pump (`pal_ui_request_view`), safe from the command task.
- Animation: `uishow lock [FRAME]`, `uishow home anim:NAME [FRAME]`, `uishow splash [STAGE]`, `uishow animsheet NAME [PAGE]`,
  and the `anim` command (`list`, `info`, `play`, `sheet`, `bench`). Golden: `home-anim-*`, `splash-stage-*`, `sheet-*`.
- Golden: the scenes `home-*`, `launcher-*`, `lock`, `control`, `notifications*`, `gallery` in
  `forge/tools/golden-scenes.json`. Update only this part: `bun forge/tools/golden.ts update home,launcher,lock`.
  The second argument is a list of scenario name prefixes.

## Transition to the desktop contract

`ui_wire.c` uses the APIs that already exist in the tree (`pal/overlay.h`, `pal/desktop.h`):

- Home is the desktop home (`pal_desktop_set_home("home")`), so Fn+Q and Esc return to home.
- The lock screen is registered as the lock view (`pal_desktop_set_lock_view("lock")`). `pal_ui_lock_now` calls
  `pal_desktop_lock()`, `pal_ui_unlock` calls `pal_desktop_unlock()`.
- Keys on home are bound (`pal_desktop_bind_home_key`): ENTER opens the launcher, UP opens control, DOWN opens
  notifications. All three are shown as opaque overlays; ESC closes them through the overlay stack, or opens home if
  it is not an overlay.
- Screens with animation use `obscured` and `exposed` (home).

While locked, every key goes to the lock screen (`desktop.c`), the cycle key included.
Sound effects and audio status do not exist yet, so the volume slider stays disabled.

## UI scale (`ui.scale`)

All kui screens are drawn on a logical 1-bit canvas, then enlarged to the panel. The scale is one for all screens (`kui_scale_q()`,
`system/include/pal/kui.h`), in units of a quarter panel pixel:

| Choice | Logical canvas on hhv2 (320x240) | Notes |
| --- | --- | --- |
| 1x | 320x240 | the most content, the smallest pixels |
| 1.25x, 1.5x, 1.75x | 256x192, 213x160, 182x137 | uneven pixel widths (nearest neighbour) |
| 2x | 160x120 | hhv2 default (`pal_board_desc_t.ui_scale_q = 8`), the screens' design size |
| 2.25x, 2.5x | 142x106, 128x96 | zoom in; 2.5x is the largest |

Below 128x96 the layouts designed for 160x120 (launcher, Control Center, home) no longer fit, so 3x is not
offered. The status bar and legend bar are logical rows, so their physical height follows the scale (`kui_top_px`,
`kui_bottom_px`, `pal_legend_h_px`). A new screen uses `kui_canvas_init(&c, w, h, kui_scale_q())`, converts physical coordinates
with `kui_lg`/`kui_ph`, and takes the width from `c->w` (not 160). The canvas is 9.6 KB, so it is declared with `NEMA_EXT_BSS`.

Set in Settings > Display > UI Scale, or `setting ui.scale 1.5x`; stored in `/data/ui` (`scale=`). Not yet scaled:
the notification screen (still an old widget, not kui) and the terminal (physical font).
