# Desktop environments

> **Being replaced.** This page describes the current hook-based model. ADR-0010 (`docs/adr/0010-system-console-and-desktop-sessions.md`)
> replaces it with a system console under whole desktop sessions, migrated in phases P1 to P6. P1 (the system console) is done and is
> described below. Known faults until P2 is done: shell screens such as Control and Notifications are the classic ones under every
> desktop, and leaving them opens the classic home.

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

## What a desktop is

```c
typedef struct {
    const char *id, *title, *about;
    bool (*start)(void);  /* register its views once, set its chrome, home view and home keys */
    void (*stop)(void);   /* undo what the next desktop will not set itself; may be NULL */
} pal_desktop_env_t;

typedef struct {          /* pal_desktop_set_chrome(&chrome); NULL is classic */
    bool hide_bar, hide_legend;          /* the compositor draws neither */
    int top_px;                          /* rows kept for the desktop's own bar: views start below, flushes never touch them */
    bool (*key)(const pal_key_t *k);     /* every key after power and lock, before the compositor's global keys */
    bool (*owns_screen)(void);           /* true while the desktop presents the whole screen itself */
    void (*frame)(uint32_t now_ms);      /* once per loop on the desktop task, after the compositor's frame */
    int32_t (*next_ms)(uint32_t now_ms); /* when frame wants to run again */
} pal_desktop_chrome_t;
```

A desktop that draws in full colour sends RGB565 images straight to the panel with `pal_display_present_rgb565` (a panel driver opts in
through `present_rgb565` in `pal_display_ops_t`; hhv2 and both simulators do). It reads the bar's data with `pal_desktop_status()`, at
most once a second: the battery is an ADC sample.

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

Under Athena, what is not a bound menu keeps the classic drawing below Athena's bar: Files, Logs, Monitor, the Terminal, and the dialogs,
pickers and on-screen keyboard that a Settings row opens (while one is open, the page under it is drawn the classic way).

## Athena

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
