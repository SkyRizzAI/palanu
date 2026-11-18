# ADR-0010: A system console under desktop sessions; a desktop is a whole session, not a skin

Status: accepted (18 November 2026); P1 to P6 done (19 November 2026); §7 and §8 amended (below). Supersedes the desktop part of ADR-0009; the LVGL and RGB565 parts of ADR-0009 stay.

## Context
ADR-0009 added desktop environments as a hook table (`pal_desktop_chrome_t`) on a compositor that is itself the classic desktop. Testing
Athena on hhv2 showed the model is wrong:

- The compositor owns the classic shell. Home, launcher, control, notifications, lock and splash are registered for every desktop
  (`system/src/ui_wire.c`), and `control`, `notifications`, `launcher`, `home` and `lock` are listed as apps in `apps/apps.toml`. Under
  Athena, opening Control shows the classic control screen.
- Screens leave by naming a classic view. `pal_desktop_open("home")` appears in `ui_control.c`, `ui_notify.c` and `launcher.c`, and the
  compositor's Esc fallback opens `D.home`. Leaving an app under Athena lands on the classic home.
- Settings mixes system settings (Wi-Fi, display, Link, apps, about) with classic-only ones (theme, font, asset pack, wallpaper, launcher
  skin). `pal_menu_bind` only restyles the classic menu; its pickers and dialogs stay classic.
- There are two pixel worlds: the 8-bit indexed canvas and Athena's RGB565 frame. Dialogs, toasts, the on-screen keyboard, the input menu
  and pickers exist only in the 8-bit world, so they look classic under Athena. Athena copies its frame back into the canvas with dither
  to sit under them, and the result looks pixelated.
- Switching desktops leaves the old one resident: its views stay registered and are pumped every loop, and the 192 KB LVGL pool stays
  allocated after Athena stops.
- A desktop sees keys after the compositor has handled some of them.
- The "console" desktop is only the Terminal app at full screen inside the same compositor. When the compositor or a desktop breaks, there
  is no screen left to use. Boot messages go only to the serial port. The boot splash is a classic overlay over the classic home.
- The app API (`api/palanu/`) has no graphical calls, so how an app looks under each desktop is not defined at the API level. Built-in GUI
  apps (Files, Logs, Monitor) draw with classic kui widgets.

The owner wants the Linux model:

- A text console always exists and shows the boot.
- A desktop is chosen and started as a session.
- Apps run inside the session and return to it when they exit.
- Each desktop has its own shell apps.
- System settings exist in every desktop.

## Decision

### 1. Three layers
| Layer | Owns | Never does |
|---|---|---|
| System | the screen frame, input routing, power, services, the system console, the session manager, the settings schema, the PIN, data services (Wi-Fi, battery, notifications as data) | draw a desktop's look |
| Session (a desktop) | home, launcher, app switcher, control and notification screens, lock greeter, status bar, global keys, the shell services in §5, its Settings app, its Terminal app, how app surfaces are framed | reach another session's views |
| App | its content | name a view of a desktop, or draw outside its surface |

### 2. The system console (the equivalent of tty1)
- `system/src/console.c` draws the shared terminal engine (`pal_term`) with a Spleen font straight into the screen frame. It does not
  use the theme, kui, LVGL or any session code.
- It shows the boot log, panics, the safe-mode notice, and a login shell (the equivalent of getty).
- The supervisor prints one status line per service: `[ OK ] radio-wifi`, `[FAIL] link-ws (reason)`, `[WAIT] ...`.
- It is always reachable. Ctrl+Alt+Esc switches between the console and the running session. The input layer handles it before the
  session sees any key, so a session cannot take it.
- The Terminal app a session offers is a separate thing, the equivalent of a terminal emulator inside a desktop. It shares the same shell
  and terminal engine.

### 3. Boot and targets
1. At power on, the console is in front and the log runs.
2. A boot splash owned by the system may cover the log while `boot.quiet` is on (the default). Any key reveals the log, as in Plymouth.
   The classic splash animation moves into the classic session.
3. When the services are up, `boot.target` decides:
   - `console`: stop at the console's login shell.
   - `desktop`: the session manager starts the session named in `desktop.env`.
4. A session that fails to start or crashes returns to the console with a message. The system does not silently start a different
   session.
5. Safe mode always stops at the console.
6. The `console` desktop is removed. "No desktop" is `boot.target = console`.

### 4. The session lifecycle
- One session runs at a time. `pal_session_ops_t` replaces `pal_desktop_env_t` and `pal_desktop_chrome_t`. It has `start`, `stop`, the key
  hook, frame presentation, and the shell services in §5.
- Views, keys and memory are registered under the session's owner. `stop` unregisters all of them and frees all of them; the session
  manager checks the owner's counts afterwards. Switching sessions means stopping one completely and then starting the other. The LVGL
  pool is allocated with `nema_alloc` at start and freed at stop, not kept in static BSS.
- The session is the root of navigation. An app leaves with `pal_app_exit()` or by returning "not handled" for Esc. The session then
  decides where to go (its home, or the previous app). No code outside a session names a session view.
- Key order: system keys (console switch, power) first, then the lock, then the session's global keys, then the front app.
- The classic shell (home, launcher, applist, control, notifications, lock, splash animation, input menu) moves into the classic session.
  These screens stop being apps in `apps/apps.toml`. The manifest gains `desktops = [...]` for apps that belong to one desktop only.
- The id stays `classic`. Product names of other projects are not used in the repository.

### 5. Shell services are a contract every session implements
`dialog` (alert and confirm), `toast`, `pick` (choose one of a list), `text` (text entry, which is where the on-screen keyboard lives),
`busy` (progress), and the notification banner.

- The system API calls the active session's implementation.
- The console provides text versions of all of them, so they work without any desktop.
- A session that leaves one out gets the console's text version, never another desktop's drawing.

### 6. One screen frame
- The system owns one RGB565 frame (320x240x2 = 150 KB in PSRAM) and flushes its dirty rectangles to the panel.
- The 8-bit indexed canvas becomes a classic-session detail. Classic draws its canvas and converts it through its palette when it
  presents, as the panel path does today.
- The console and Athena write RGB565 directly.
- Remote mirroring reads the system frame, so it shows every session.
- `pal_display_present_rgb565` and `present_rgb565` stay. The dither copy from Athena into the canvas is removed.

### 7. Settings are a system schema
- `settings_kv.c` and the table in `settings_api.c` merge into one schema of descriptors. Each descriptor has: key, kind (bool, int range,
  choice, text, action, PIN), label, group, help, the board capability it needs, and the owner (`system` or a session id).
- Every session ships its own Settings app. That app lays out every `system` group from the schema with its own components, then adds the
  groups it owns (for example wallpaper and launcher skin for classic, accent colour for Athena).
- A test walks the schema in every session and fails when a `system` key is not reachable. A system setting cannot exist in one desktop
  and be missing in another.
- `pal_menu_bind` is removed once both Settings apps read the schema.

**Amendment (P5).** One Settings app serves every session, in the session's look, instead of a Settings app per session. The
reason is the dependency direction: the feature pages (Wi-Fi networks and joining, Remote pairing, Apps, Services, Storage, About)
belong to the apps layer and run background jobs there, and a session lives in the system layer below it; a second copy of each page
per session would double the code and drift. What §7 is for still holds:

- every key has one descriptor in `system/src/settings_schema.c` with its owner (the system, `classic` or `athena`);
- a row that shows a key of another session, or a page that belongs to another session (`owner` on the row), is left out, so the
  classic Appearances page does not show under Athena;
- Settings > Desktop lists the sessions (switching now) and lays out the running session's own keys from the schema
  (`pal_settings_schema_rows`), so Athena's theme shows only under Athena;
- `tests/unit/test_settings_reach.c` walks Settings under each session and fails when a system key cannot be reached, or when a
  key of another session shows. It found that brightness was only in the classic control center; Display has it now.

`pal_menu_bind` stays: it is how a session draws a menu (a form) in its own look, and P6 builds the form API on it.

### 8. How apps look under each desktop
The manifest declares `ui`:

| `ui` | What the app provides | How sessions show it |
|---|---|---|
| `tty` | text on a TTY | inside the session's Terminal app; the same everywhere |
| `form` | pages, lists, fields and actions as data (a public form API at a new API level, grown from today's menu rows) | each session lays it out with its own components, so the app adapts automatically |
| `canvas` | pixels in an RGB565 surface the session hands it (a window or the full screen) | the same pixels everywhere; the session only frames it |

- Apps do not carry per-desktop code. Two code paths per app would double every author's work and break when a third desktop appears.
- An app that only makes sense in one desktop says so with `desktops = [...]`.
- Built-in apps follow the same rule:
  - Settings: per session, as in §7.
  - Files, Logs and Monitor: become `form` apps.
  - Text viewer: becomes `form`, with a text-block field.
  - Terminal: per session, on the shared engine.

**Amendment (P6).** The public API is level 6 (`api/palanu/ui.h`): `pal_form_*` and `pal_canvas_*`, records instead of structs
and waiting calls instead of callbacks, as the app ABI requires. A form is kept by the system (`system/src/appui.c`) as a menu page
bound to its view, so every session draws it with what it already draws menus with. A canvas is copied into the screen frame after
the compositor's frame. The manifest has no `ui` field: `kind = "gui"` means the app opens a form or a canvas, and which one is its
choice at run time. Monitor is a form now. Files, Logs and the text viewer keep their own layouts for now; they are canvas-style apps
under every session until they are rewritten as forms, which is follow-up work.

### 9. Lock and PIN
- The system owns the PIN: the stored hash (`security.pin`, already in `ui_prefs.c`), verification, and the retry delay.
- A session draws its own lock greeter and calls the system to verify, the way a display manager's greeter does.
- The console asks for the same PIN before its shell when a PIN is set.
- There are no user accounts. The device has one owner.

### 10. Invariants with tests
These run in the host test suite (`./pal test`) for every registered session:

- Esc and `pal_app_exit` from every app land on that session's home.
- No view of another session is registered while a session runs.
- After `desktop use X` then `desktop use Y`, the owner counts of X are zero.
- Every `system` setting is reachable in that session's Settings.
- Every shell service is drawn by the session or by the console, never by another session.
- A session that fails at start ends at the console.
- Ctrl+Alt+Esc reaches the console from any screen, including the lock.

## Migration, in phases
Each phase ends with `./pal check`, `./pal test`, golden images and a run on hhv2.

| Phase | Work |
|---|---|
| P1 | System console, boot log status lines, `boot.quiet` splash owned by the system, console switch key, session failure falls back to the console; remove the `console` desktop |
| P2 | Session lifecycle and ownership; key order; move the classic shell into the classic session; `pal_app_exit`; remove every named home; manifest `desktops` |
| P3 | Shell services contract with classic, Athena and console implementations; remove the dither copy |
| P4 | One RGB565 screen frame; classic presents through its palette; mirroring from the frame |
| P5 | Settings schema; Settings apps for classic and Athena; remove `pal_menu_bind` |
| P6 | Public `form` and `canvas` API (`PAL_SINCE`, raise `PAL_API_LEVEL` and `PAL_PAPP_API`); move Files, Logs, Monitor and the text viewer to `form` |

## Consequences
- This is a large refactor. Most classic golden images keep their pixels, but the scenes that cross sessions change, and some screens
  move between files.
- The firmware image is already at 91% of the OTA slot. Athena implementations of the shell services and Settings add code, so the
  decision on trimming LVGL or enlarging the OTA slot must be made before P3.
- Internal RAM stays tight. The new frame, the LVGL pool and session state go to PSRAM. The console's terminal state goes to PSRAM except
  for what a panic path needs.
- The `form` API becomes part of the app ABI and only grows (hard rule 6), so its first version is kept small.
- Until P2 is done, the known faults described in the context section remain in the firmware.
