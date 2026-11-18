# Desktop environments

> **Being replaced.** This page describes the current hook-based model. ADR-0010 (`docs/adr/0010-system-console-and-desktop-sessions.md`)
> replaces it with a system console under whole desktop sessions, migrated in phases P1 to P6. P1 (the system console), P2 (whole
> sessions), P3 (shell services), P4 (one screen frame) and P5 (settings schema) are done and described below.

PalanuOS is built like a Linux system. The base system is the kernel, the services and the **system console** (below), a shell on a
text screen of its own. Everything graphical around the
apps is a **desktop environment**: the status bar, the global keys (Fn twice, the cycle key), the home screen, the menus those keys open,
and the way apps are listed and launched. The compositor underneath (`pal/desktop.h`: views, overlays, dialogs, lock, power) is shared
plumbing, like a display server; a desktop decides what surrounds it. The registry is `pal/desktops.h` (`system/src/desktops.c`).

## The system console

The console is the system's own text screen, the equivalent of tty1 (`pal/console.h`, `system/src/console.c`). It draws with its own
canvas, fixed colours and the Spleen 6x12 font, never with the theme or a desktop's code, so it still works when a desktop does not.

- **Boot.** The console is in front from power on. It prints the version and board, `[  OK  ] Started NAME.`, `[FAILED] Failed to start
  NAME: reason` and `[ WAIT ]` lines from the service supervisor, and every warning and error logged during the boot. With
  `boot.quiet` on (the default; Settings > Developer > Quiet boot) a plain splash covers the log; any key shows it. Safe mode never
  shows the splash.
- **Hand-off.** When the boot stages are done (at least 2 s, at most 5 s), a running desktop session takes the screen. With no
  session (`boot.target` console, safe mode, an unknown `desktop.env`, or a desktop that failed to start) the console stays, with the
  reason on screen, and logs in.
- **Login.** With a PIN set (the lock PIN, `security.pin`) it asks `PIN:` before the shell; a wrong PIN waits 3 s. Without one it starts
  the shell at once. The shell is respawned when it exits, and locking the device logs the console out.
- **Switching.** Ctrl+Alt+Esc switches between the console and the running session, from any screen, the lock screen included. The
  input layer gives this key to the console before the session sees it. Shift+PgUp/PgDn scroll its history.

## Using the sessions

In the shell (the console, the Terminal, the serial console, or a remote shell):

```
desktop                 # list the desktops; * marks the running one
desktop use athena      # stop the running session, start this one, keep the choice in desktop.env
desktop start athena    # the same
desktop stop            # stop the running session: the console takes the screen
```

Typed in the console, `desktop use` also gives the screen to the new session. The choice is applied again at boot. An unknown name is
refused and nothing changes. A desktop that fails to start leaves no session running and the console in front; no other desktop is
started in its place. Safe mode starts no session and leaves `desktop.env` for the next normal boot. "No desktop" is
`boot target console` (`boot.target`), not a desktop.

| id | What it is | Bar | Fn twice | Cycle tap / hold | Home |
|---|---|---|---|---|---|
| `classic` | the themed pixel desktop | themed status bar, legend bar | input menu | switcher / home | clock; Enter launcher, Up control, Down notifications |
| `athena` | full-colour LVGL desktop | its own bar on every screen | its Keyboard sheet (on-screen keyboard, shortcuts, Settings) | its Recent apps sheet / home | clock and an app grid |

Shell keys (Ctrl+C, Ctrl+D, history, Tab) belong to the TTY and work the same under every desktop.

## What a session is

```c
typedef struct {
    const char *id, *title, *about;
    bool (*start)(void);  /* register its screens, set its chrome, home view, lock view and home keys */
    void (*stop)(void);   /* give back what it took outside the compositor (memory, libraries); may be NULL */
} pal_desktop_env_t;
```

- **Whole sessions.** What a session registers during its `start` is its own. Its stop takes every one of its screens away (out of the
  overlays, the recent views and the registry) and clears everything it set on the compositor: chrome, home, lock view, home keys,
  the rows kept for its bar. Switching is a full stop, then a full start, on the screen loop's task (`desktop use` from the shell waits
  for it). No screen of one session exists while another runs; `tests/unit/test_sessions.c` checks it for the real sessions.
- **Classic** registers home, lock, launcher, app list, control center, notifications and the splash at its start, and handles Fn twice
  (input menu) and a tap of the cycle key (switcher) in its key hook. **Athena** registers its home and its own lock screen
  (`athena-lock`: clock, date, PIN dots; the PIN and its check stay the system's), takes LVGL's pool and its frame at its start and
  gives them back at its stop (`lv_deinit`).
- **Leaving a screen.** Esc that a screen does not take, the home key, and `pal_app_exit()` all go to the running session's home. No
  code outside a session names a session's screen. With no session, Esc and the home key go nowhere and `lock` is refused.
- **Key order.** The system's (Ctrl+Alt+Esc, the screen wake), then the lock screen, then the session's keys (`chrome.key`, then its home
  keys), then the top overlay, then the front view.
- **Apps per session.** An app manifest may say `desktops = ["classic"]` (`apps/apps.toml`; in a package, `"desktops": "classic"`): the
  launchers of other sessions do not list it. Control, Notifications, the app list, the launcher, home and lock are classic's.
- `lock` (shell) locks the screen of the running session.

```c
typedef struct {          /* pal_desktop_set_chrome(&chrome) in start; cleared at stop */
    bool hide_bar, hide_legend;          /* the compositor draws neither */
    int top_px;                          /* rows kept for the session's own bar: views start below, flushes never touch them */
    bool (*key)(const pal_key_t *k);     /* every key after the system's and the lock's */
    bool (*owns_screen)(void);           /* true while the session presents the whole screen itself */
    void (*frame)(uint32_t now_ms);      /* once per loop on the desktop task, after the compositor's frame */
    int32_t (*next_ms)(uint32_t now_ms); /* when frame wants to run again */
} pal_desktop_chrome_t;
```

**Shell services (P3, `pal/shell.h`).** The dialog, the picker, the text entry, the busy box and the on-screen keyboard keep their
behaviour in the system (state, keys, the answer to the caller) and describe themselves as data (`pal_shell_view_t`: kind, title, text,
items, selection, caret, keyboard caps). How they look is the session's:

| Session | Shell services | Toasts |
|---|---|---|
| classic | the overlays' own drawing with the theme's components | the themed band under the bar |
| athena (`chrome.services`) | a modal: the screen under it (copied from the screen frame), dimmed, and a rounded card; the keyboard is a sheet of key caps | in its status bar, over the clock |
| none | the system console: a framed text box over the bottom of the terminal, and the keys go to the service | printed nowhere |

`uidemo dialog | input | picker | busy SECONDS | toast` opens each one (a test aid).

**Settings (P5).** Every settings key has one descriptor in the schema (`pal/settings_schema.h`): kind, range or choices, label,
group, help, the board capability it needs, and its owner: the system, `classic` or `athena`. One Settings app serves every session in
the session's look. A row of another session's key, or a page that belongs to another session (`owner` on a menu row), is left out:
the classic Appearances page shows only under classic. **Settings > Desktop** switches the session (now, and at every start) and shows
the running session's own keys, laid out from the schema; under Athena that is its theme. `test_settings_reach` checks under every
session that each system key can be reached and that no other session's key shows.

**The screen frame (P4).** The system owns one RGB565 frame the size of the panel, in PSRAM (`pal_display_frame`). Everything reaches the
panel through it: the compositor's 8-bit canvas (through the theme's palette), the system console (through its own fixed palette) and a
full-colour session (`pal_display_present_rgb565`, copied as it is). Remote mirroring reads the frame, so a client sees Athena and the
console in full colour (`docs/PLP.md`, RECT565); an older client gets the frame mapped onto the palette's colour cube. A panel driver
without `present_rgb565` keeps the 8-bit path and has no frame (no full-colour session then). A session reads the bar's data with
`pal_desktop_status()`, at most once a second: the battery is an ADC sample.

## How apps look under each desktop (decision, ADR-0009)

An app never draws a status bar or handles the global keys; it gets an area and keys. How much of the desktop's look it takes depends on
how it draws:

1. **TTY apps** (shell commands, `.papp` tty apps) run in the Terminal. They look the same everywhere; the desktop only frames the Terminal.
2. **Toolkit apps** draw through the system UI toolkit (`pal/ui.h`, the theme's component table: title bar, list rows, dialogs, text
   fields, toasts, `pal/menu.h`). They take the look of the active theme. A desktop may ship a theme of its own and select it in `start`;
   then every toolkit app follows the desktop without a change. This is the way to write an app that fits every desktop.
3. **Canvas apps** draw raw pixels on the canvas they are given (games, viewers, custom screens). They look the same under every desktop;
   the desktop only decides where the area is (below its bar) and handles the keys around it.

   A toolkit whose content is data goes further: a view that shows a menu (`pal/menu.h`) binds it with `pal_menu_bind(view_id, menu)`, and
   a desktop may lay the rows out itself. Athena draws bound menus as a grouped list in full colour (large title with a back chevron,
   section headers, rounded rows, switches for toggles, values and chevrons on the right); the keys still go to the view and
   `pal_menu_key`, so the behaviour is the app's. Settings binds its menu, so every Settings page follows the desktop.

Apps that need full colour and their own widgets (an LVGL app) are a later step: they need a surface API (an RGB565 buffer and a present
call for the app's area) in the app API, with its own `PAL_SINCE` level.

Under Athena, what is not a bound menu keeps the classic drawing below Athena's bar: Files, Logs, Monitor and the Terminal. The dialogs,
pickers, text entries and the on-screen keyboard they open are Athena's (P3).

## Athena

**Themes.** Athena's look is a theme of tokens (`system/src/athena/athena_theme.c`): colours (wallpaper, surface, rows, accent, text,
switches, danger, keyboard mode keys, the bar over apps) and shape (corner radii of cards, rows and icons, centred or left-aligned
titles, upper-case buttons, shadows or flat). Two ship as a demo: **Cupertino** (the default: centred titles, round corners, soft
shadows, inset grouped lists) and **Material** (flat, small radii, round icons, left-aligned titles, upper-case buttons, edge-to-edge
lists, a teal accent). The choice is the `athena.theme` setting (`cupertino` or `material`); Athena follows a change within a second
by building its objects again. A classic theme only swaps colour tokens; an Athena theme also changes shapes and layout rules, which
costs a rebuild of the objects, nothing more.


`system/src/athena/athena.c`, LVGL 9.2.2 (`third_party/lvgl`, MIT) configured in `system/src/athena/lv_conf.h`: software rendering, LVGL's
allocator on a 192 KB pool in PSRAM, the Montserrat fonts (SIL OFL), only plain objects and labels. LVGL renders one RGB565 frame the size
of the panel (in PSRAM) in direct mode.

- Home or a sheet in front: Athena owns the screen and sends its dirty areas to the panel; the compositor draws nothing.
- An app in front: the compositor draws it below Athena's 20-row bar and Athena sends only the bar. A sheet opened over an app sends only
  the sheet, so the app stays visible around it.
- Something over the home (a toast, the on-screen keyboard): the home is copied into the 8-bit canvas, dithered onto the palette's colour
  cube, so the overlay can be drawn on top.

Built into the firmware and both simulators. Cost on hhv2: about 400 KB of flash (the image is 91% of the OTA slot), no internal RAM
beyond a few hundred bytes (the pool, the frame and its state are in PSRAM), and the desktop task's stack went from 6 to 8 KB.

Golden scenes: `athena-home`, `athena-select`, `athena-settings`, `athena-settings-display`, `athena-keyboard`, `athena-recents`, `athena-app`, `console-shell`
(`forge/tools/golden-scenes.json`). Unit test: `tests/unit/test_desktops.c` (the registry and the console's hand-off).
