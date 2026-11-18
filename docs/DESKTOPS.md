# Desktop environments

PalanuOS has one compositor (`pal/desktop.h`: layers, status bar, overlays, lock, power) and any number of **desktop environments** on top of it. A
desktop environment decides what the user sees at home and how apps are listed and launched. The registry is `pal/desktops.h`
(`system/src/desktops.c`).

## Using them

In the shell (or the Terminal on the simulators):

```
desktop                 # list the desktops; * marks the active one
desktop use athena      # switch now, and keep the choice in the desktop.env setting
desktop start athena    # the same
```

The choice is applied again at boot. An unknown name is refused; a desktop that fails to start is replaced by `classic` (the fallback, found by its id).

| id | What it is | Where it is built |
|---|---|---|
| `classic` | the clock home; Enter opens the launcher, Up the control center, Down the notifications | every build |
| `athena` | an LVGL home: a clock and a grid of the launcher's apps; arrows move, Enter opens | the simulators only (`./pal sim`, `./pal web`) |

## What a desktop is

```c
typedef struct {
    const char *id, *title, *about;
    bool (*start)(void);  /* register the desktop's views once, set the home view and the home keys */
    void (*stop)(void);   /* undo what the next desktop will not set itself; may be NULL */
} pal_desktop_env_t;
```

`start` normally calls `pal_desktop_register` for its home view (once; an `EXIST` result is fine), `pal_desktop_set_home(id)` and
`pal_desktop_bind_home_key` for the keys it wants on home (a `NULL` view removes a binding, which is how Athena frees Enter, Up and Down for its grid). Everything else
stays with the compositor: Esc and the long press of the cycle key return to the desktop's home, overlays and dialogs look the same, the lock screen and
power management are shared. Register with `pal_desktops_register` before `pal_boot` (a platform) or from `pal_ui_register_all` (the classic desktop).

## Apps and desktops

A desktop lists apps from the registry (`psys_launcher_count` and `psys_launcher_get`, the same list the classic launcher uses) and opens one with
`pal_launcher_run`. Each entry already carries what a desktop needs to draw it: the display name, version, an icon id, a category, the kind and the permissions. The
`.papp` bundle format (docs/PAPP.md) has a section for the icon and one for resources. Athena does not read the icon section yet: it maps the app id to a built-in glyph, or draws the initial.
Reading bundle icons, and shortcut entries a desktop can read (a dock, a pinned list), are the next steps.

## Athena

`system/src/athena/athena.c`, with LVGL 9.2.2 (`third_party/lvgl`, MIT) configured in `system/src/athena/lv_conf.h`: software rendering, LVGL's allocator on a 192 KB static
pool, the Montserrat fonts (SIL OFL), no extras. LVGL renders into its own RGB565 frame in direct mode; `draw` copies the dirty area into the
desktop's 8-bit canvas, snapping each colour to the xterm 6x6x6 cube that every theme palette carries (slots 16 to 231), with ordered dithering so that gradients look smooth on the small panel. Athena has its own look (a gradient wallpaper, rounded gradient icons with a glyph each, light type, a translucent bar) and ignores the theme and dark mode. The glyphs are LVGL's built-in symbols, mapped by app id; an app without one shows its initial.

Athena is **not in the firmware**: LVGL's pool and code do not fit the internal RAM budget (docs/STATE.md), and the board gains nothing from it yet. The firmware build does not
compile `system/src/athena`, `third_party/lvgl` or `platform/host/libs.cmake`; `desktop` there lists `classic` only.

Golden scenes: `athena-home` and `athena-select` (`forge/tools/golden-scenes.json`). Unit test: `tests/unit/test_desktops.c`.
