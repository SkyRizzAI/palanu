# Desktop: compositor, overlays, input, dialogs, and power

The desktop infrastructure contract (T5.1, T5.4, T5.5, T5.6, T5.12, part of T3.8). The visual screens (launcher, home, lock, control
center, notification center, icons) are built on top of it and use only the APIs below. Code is in
`system/include/pal/` (public headers) and `system/src/` (implementation). Host tests are in `tests/unit/test_overlay.c`,
`test_dialog.c`, `test_toast.c`, `test_dpm.c`, and `test_desktop.c`.

## Layers

From bottom to top, in one framebuffer:

1. Front view: an app or home (`pal_desktop_home_id()`, default `launcher`).
2. Status bar (unless the top layer is `NOBAR`, or the front is `FULLSCREEN`).
3. Overlay stack (max 4): dialog, picker, text input, busy, switcher, control center, lock.
4. Toast: a small bar below the status bar, above everything.

View flags (`pal/desktop.h`):

| Flag | Meaning |
| --- | --- |
| `PAL_VIEW_FULLSCREEN` | no status bar while it is in front |
| `PAL_VIEW_HIDDEN` | not listed in the launcher |
| `PAL_VIEW_OVERLAY` | (set automatically on push) drawn over the layer below, which stays visible |
| `PAL_VIEW_OPAQUE` | covers everything below it; lower layers are not drawn while it is on top |
| `PAL_VIEW_NOBAR` | no status bar while it is the top layer; the overlay area becomes the whole screen |

`pal_view_t` adds two members at the end: `obscured(v)` (an overlay was pushed above v) and `exposed(v)` (the overlay above v
was closed). Both are optional. Positional initialization must be replaced with named initializers or given those two members.

Layer area: below the status bar, except for `FULLSCREEN` or `NOBAR`.

## Overlays (`pal/overlay.h`)

- `pal_desktop_overlay_push(v)`, `pal_desktop_overlay_pop()`, `pal_desktop_overlay_remove(v)`,
  `pal_desktop_overlay_top()`, `pal_desktop_overlay_count()`.
- Push calls `enter`, notifies the layer below through `obscured`, and does a full redraw.
- Pop calls `leave` on the overlay, then `exposed` on the new top layer, then does a full redraw.
- Every change from another task is queued to the desktop task (`NEMA_OK` means queued; queue of 8). From the desktop task
  it takes effect immediately. Before the desktop runs: immediately.
- Redraw: `pal_desktop_invalidate()` draws only the top layer (`full = false`). `pal_desktop_invalidate_all()`
  redraws everything. Layers below an opaque overlay are not drawn.

## Key order

Decisions in `desktop.c`, in order:

1. Power: the first key while the screen is off only wakes it and is discarded (see DPM).
2. Locked: all keys go to the lock view only. Globals do not apply.
3. `PAL_KEY_GOHOME` (hold cycle): close all overlays, open home.
4. `PAL_KEY_SWITCH` (short press of cycle): if there is no overlay, open the switcher. If there is, forward it to the top overlay.
5. The top overlay receives the key. An Esc the overlay does not handle closes it.
6. Home key binding (if the front is home and there is no overlay): open the bound view as an opaque overlay.
7. Front view. An Esc the front does not handle (not home) goes to home.

Home binding (`pal_desktop_bind_home_key(sym, view_id)`, `NULL` removes, max 8): data from boot code. Binding Enter to the
launcher would override Enter on home itself; set it according to the home screen.

## Navigation (`pal/nav.h`)

`pal_nav_of(&key)` maps a key to an intent: `PAL_NAV_UP/DOWN/LEFT/RIGHT` (Fn+W/S, Fn+A/D), `OK` (Enter), `BACK` (Esc),
`MENU` (Tab). The legend at the bottom of the screen (legend bar) uses actions, not keys: see `docs/LEGEND.md`.
`pal_nav_key(n)` returns the primary key of that intent (for reachability tests).

## Toast (`pal/toast.h`)

- `psys_toast(text, ms)`: posts through the notify toast queue (`source = "system"`), ms defaults to 2000 and is clamped to 100 to
  10000. No `pal_` prefix because the SDK already uses `pal_toast` for apps (`api/palanu/notify.h`).
- The desktop takes toasts from the queue (4) while the notify sink is active. Only one toast is shown; text is cut to 40 characters.
- Look: a full-width band below the status bar (`Toast`, corner radius 1, 38 physical px). The text is drawn in
  paper color on the ink band so it stays readable (`kui_toast_band`).
- Timing is based on `pal_toast_next_ms`. There is no polling.

## Dialog, text input, picker, busy (`pal/dialog.h`)

All non-blocking calls can be made from any task; answers arrive through a callback on the desktop task.

- `pal_dialog_confirm(title, text, ok, cancel, cb, ctx)`: two buttons, Cancel on the left (initial focus), OK on the right
  like a confirm modal; Enter = the focused button (so Enter alone cancels), Esc = cancel, Fn+A/D selects. Max 2
  dialogs. Cancelled (`ok = false`) if the overlay is removed by someone else (home, etc.).
- `pal_dialog_confirm_wait(...)`: a blocking version for other tasks. From the desktop task it returns `false` and logs an
  error (it does not hang).
- `pal_textinput_open(title, initial, max_len, mask, cb, ctx)` and `pal_textinput_wait(...)`: one editor. ASCII characters
  0x20 to 0x7E; Backspace, Delete, Fn+A/D, Home, End; Enter = OK; Esc = cancel. Text scrolls if it is wider than the
  box. `mask` shows `*`, the result stays the original text. The caret blinks (500 ms, via tick).
- `pal_picker_open(title, items, n, cb, ctx)`: max 8 items, the pointer is kept (not copied). Arrows wrap around.
  The callback receives the index, or `-1` on cancel.
- `pal_busy_show(text)` / `pal_busy_hide()` / `pal_busy_showing()`: a spinner that swallows all keys.

Rule: waiters (`*_wait`) are only called from other tasks. Do not call them from a callback or view on the desktop task.

## Theme components (`pal/theme.h`)

`pal_components_t` adds at the end: `dialog_box`, `text_field`, `toast`, `spinner`. Built-in themes that leave a member
NULL inherit the default (`system/src/theme_widgets.c`, called `pal_components_fill`). The "Lines" theme uses the defaults for
all four.

## Status bar (`pal/desktop.h`)

- `pal_status_t` adds `ble` and `sd` (both at the end). Boards and the sim fill them: `ble` from the radio policy, `sd`
  has no card reader yet (false).
- Status items from an owner: `pal_status_item_register(&item)` (max 12) with `draw(c, t, x, h, st)`, which returns the width
  (0 = not shown). Items are drawn in order starting at `PAL_STATUS_ITEMS_X` (34). `pal_status_changed()` requests a redraw of the
  bar from any task.
- Built-in items (`statusitems.c`): a dot and the unread notification count (`psys_notify_unread_count`), a padlock (when
  locked), BLE, SD card.
- The battery is not shown while charging (there is no sensor).

## Lock and power (`pal/dpm.h`, `pal/desktop.h`)

- States: ACTIVE (screen on, unlocked), SLEEP (backlight off), LOCKED (lock view shown; screen on or off).
- Times: `dpm.sleep_ms` (default 60000, 0 = never); `dpm.lock_ms` (default 0 = never), counted from when the screen
  went to sleep. Configured through the `display.sleep` and `display.lock` (seconds) table in `settings_api.c`, stored in `/data/settings`
  as `dpm.sleep`, `dpm.lock` (seconds).
- The first key during sleep: wakes the screen and is discarded. Non-key activity (Link) resets the timer without waking
  (`pal_desktop_activity()`).
- `pal_desktop_set_lock_view(id)` registers the lock view. `pal_desktop_lock()` pushes it as an opaque overlay and
  sends all keys to it (Esc does not close it). `pal_desktop_unlock()` is called by the lock view when the PIN is correct or a
  key matches. It fails (NOENT) if no lock view has been registered. Lock from another task is queued.
- `pal_desktop_locked()`: for Link. **link.c** must check this before forwarding an input channel frame (`CH_INPUT`):
  if locked, drop the frame without `pal_input_inject`; still call `pal_desktop_activity()`.
- PIN: `pal_dpm_pin_set(pin)` (4 to 16 printable ASCII characters; `""` clears it), `pal_dpm_pin_check(pin)`,
  `pal_dpm_pin_is_set()`. Stored as `security.pin=<salt hex>:<sha256 hex>` (salted SHA-256, salt from the TRNG).
- There is no DIM (optional, not in settings yet).

## App switcher (`overlay_switcher.c`)

Short press of cycle: a list of views that have been in front (most recent first), starting from the previous app. Enter opens the selection,
Esc closes. Memory per view: bytes of the owner whose name equals the view id (e.g. `terminal`); otherwise a `-` mark.
`pal_desktop_switcher_show()` is for apps and `uidemo`.

## Not yet done

- Visual screens (lock, home, control center, notification center) and icons: work for the visual agent.
- Boot code does not register a home binding yet: there is no target view available.
- `link.c` does not use `pal_desktop_locked()` yet (see above).
- Measurements on the board: not yet (orchestrator).

## Tests

`./pal test` runs the tests above. `./pal golden check` uses the scenes `dialog`, `input`, `toast`, and `switcher`
(via `uidemo` in the terminal, in the default and dark themes) in `forge/tools/golden-scenes.json`.
