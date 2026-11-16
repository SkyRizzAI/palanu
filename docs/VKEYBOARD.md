# Virtual keyboard (full screen)

The hhv2 physical keyboard has 39 keys and Nm/Fn layers. Symbols such as `! ? @ # $ % ^ & *` and capital letters are sometimes hard to
type, for example when entering a Wi-Fi password. The virtual keyboard gives a full screen with all keys, and is
used in two ways: straight into the app in front of it (live mode), or as a text entry screen for system flows
(entry mode).

Code: `system/include/pal/vk.h` (API), `system/include/pal/vk_layout.h` (pure rules), `system/src/vk.c` (controller),
`system/src/vk_layout.c` (key table, focus, shift, translation), `system/src/vk_draw.c` (drawing), `apps/cmd_vk.c`
(the `vkeyboard` command). Tests: `tests/unit/test_vk.c`, `test_tty_edit.c`, `test_sh_vk_edit.c`, and additions in `test_input.c`.

## Trigger
- **Fn tapped twice quickly** produces `PAL_KEY_VKB` (`system/src/input.c`). A single Fn tap must be shorter
  than 300 ms, the gap between two taps under 350 ms, and no other key may be pressed while Fn is held (so
  Fn+W is still an arrow). A third tap starts a new count, so a triple tap does not open twice.
- The `vkeyboard` command (`open TEXT [layer]`, `close`, `entry TITLE [mask]`) and the simulator step `--key vkb`.

## Fn twice opens the input menu, not the keyboard
`PAL_KEY_VKB` is intercepted in `handle_key` (`system/src/desktop.c`) before the overlay stack receives keys, then forwarded to
`pal_input_menu_fn_double()` (`system/src/input_menu.c`, `pal/input_menu.h`):

| State | Fn twice | Fn once |
|---|---|---|
| on-screen keyboard open | closes it | nothing |
| menu open | closes the menu, like Esc | moves focus down one row (same as the down arrow) |
| otherwise | opens the menu | nothing |

The same gesture opens and closes the menu. A single Fn tap is reported as `PAL_KEY_FNTAP` only while the menu is open
(`pal_input_report_fn_tap`, turned on by the menu's `enter` and off by its `leave`), so other apps never see it. The first
tap of a quick pair still moves focus first, then the second tap closes the menu: the focus shift is
not visible because the menu disappears right away.

The menu has five rows, focus always starts on **Keyboard**: `K` Keyboard (opens the on-screen keyboard), `S` Symbols (locks the Nm layer
on the physical keyboard), `N` Numbers (locks only the number keys: W E R S D F Z X C and space become 1-9 and 0), `C` Caps Lock,
`T` Ctrl (applies to the next single key). Arrows or a single Fn tap move focus, Enter selects, the letter on a row
selects it directly, Esc or Fn twice closes. A mode that is on is marked with a small box in the status bar (`A` Caps, `#` symbols, `1` numbers, `^`
Ctrl) and is turned off by selecting its row again. Shift twice quickly also toggles Caps Lock. These modes are in
`system/src/input.c` (`pal_input_set_mode`); tests in `tests/unit/test_input.c`. Shortcut for the on-screen keyboard: Fn twice, Enter (two gestures).

The initial focus of the on-screen keyboard and after a layer change is the layer key at the bottom left (`123`, `abc`, `#+=`), not the middle of the second row:
to type symbols or numbers, Enter is enough, and Enter again moves to the next layer (`vk_focus_home`).

## Appearance (`system/src/vk_draw.c`)
- A title on top (top left, FONT_REG8), a separator line, then the input row with a `_` cursor. In entry mode, the title is the
  prompt sentence (e.g. `Password: <ssid>`). Another separator line above the keys.
- Four rows of keys on a 16 px grid (2 half-units = 16 px, 1 half-unit = 8 px), rows of 13 px, no gaps. Every
  row totals 20 half-units, so columns line up across all rows. Labels use FONT_REG8.
- The focused key has its colors inverted with rounded corners (radius 2). Caps (⇧) shows `v` (lowercase) or `^` (uppercase).
- Entry mode: the bottom row is `123 SPACE OK X` (X cancels). An entry with a password (mask) adds a `show` key so that the
  password can be seen.
- Live mode (keyboard over an app) uses the PalanuOS key table (including the ctl layer) with the same drawing.
- The screen starts at the top left corner (the `q` key). The status bar stays visible above the keyboard.
- All colors come from theme slots (`PAL_C_*`), so dark mode and other themes need no special code.

## Layers
| Layer | Content |
|---|---|
| abc (ABC when shift is active) | q w e r t y u i o p / a s d f g h j k l ' / ⇧ z x c v b n m ⌫ / 123 , space . ↵ |
| 123 | 1 2 3 4 5 6 7 8 9 0 / - / : ; ( ) $ & @ " / ' ! ? + * % = _ ⌫ / abc #+= , space . ↵ |
| #+= | # _ = ~ ` \| \\ ^ < > / [ ] { } & ( ) ; : ' " / $ / * ? ! ~ \| \\ ⌫ / abc 123 , space . ↵ |
| ctl | Esc Tab Ctrl Alt Ins Del Home End PgUp PgDn / ← ↑ ↓ → / ^C ^D ^Z ^L ^U ^W ^A ^E ⌫ / abc 123 #+= space . ↵ |

- Shift: one tap = one letter (ABC), two quick taps (400 ms) = caps lock (all capitals).
- The physical Tab and the on-screen key cycle through: abc, 123, #+=, ctl (in entry mode, ctl is skipped).
- All ASCII characters the physical keyboard can send are in one of the layers (tested).
- F1-F12 are **not there**: `pal_keysym_t` has no function keys yet and `pal_key_to_bytes` does not translate them yet.
  Deferred until there is a real need.
- The app-cycle key (⟳) has no on-screen counterpart; the Nm and Fn keys do not either (Nm/Fn stay physical).

## Physical navigation
- Fn+W/A/S/D (arrows): move focus. Wraps at row edges and between rows. The column is remembered when moving up/down.
- Fn+Q/E/R/F (Home/End/PgUp/PgDn): focus to the start/end of a row, or the top/bottom row.
- Physical Enter: after the cursor has been moved with the arrows, Enter presses the focused key (in both live and entry mode, so `!` in a Wi-Fi password can be typed). After typing on the physical keyboard, Enter acts like a normal Enter: in entry mode it confirms, in live mode it sends Enter to the app. The on-screen OK key always confirms. The on-screen ↵ key (live mode) sends Enter to the app and then **closes** the keyboard.
- Esc or Fn Fn: close. In entry mode, Esc cancels.
- Character keys, Backspace, Delete, and others from the physical keyboard still go straight to the app, and appear in the box.

## Sending to the app (live mode)
Every on-screen key produces the same `pal_key_t` as the physical keyboard: a character with `cp` (uppercase if
shift), mods `SHIFT/CTRL/ALT` (sticky Ctrl/Alt are released after one key), and `PAL_KEY_*` for Enter, Backspace,
Tab, Esc, arrows, Home/End, PgUp/PgDn, Delete, Insert. Delivery goes to the front view (`pal_desktop_front_id()`, not an
overlay) through `pal_desktop_view(id)->key`.

The text box follows the app through a **text source**: a view registers a `pal_vk_source_t` with `pal_vk_register_source(id,
src)`. The terminal (`apps/terminal.c`, id `"terminal"`) reads the line being edited in the active tab from the TTY. The shell
(`apps/sh_edit.c`) publishes its line and cursor every time they change (`NEMA_TIOC_SETEDIT`), and clears them when the line
is submitted or the read finishes (`NEMA_TIOC_CLREDIT`). In raw mode from another program, the edit state is also cleared.
When a key is pressed, the box is refreshed every 50 ms for 300 ms (the shell echoes characters asynchronously), then
every 250 ms. If there is no source, the box shows the text typed in this keyboard session.

## Entry mode (text for system flows)
`pal_vk_edit(&e)` blocks the calling process until OK (returns the length, >= 0) or cancel (`PAL_VK_CANCEL`, -1).
`pal_vk_edit_async(&e, cb, ctx)` is for callers on the desktop task; `cb(len, ctx)` is called when it finishes.
- Nothing is sent to an app. The box is the source of truth.
- `e->mask`: dots instead of the text, with a `show`/`hide` key (an eye key) to view it.
- `e->allow_empty = false`: OK with empty text is rejected (the keyboard stays open).
- The maximum length is `e->cap - 1` (limited to 256 bytes).
- Return values: `NEMA_ERR_NOSYS` if there is no screen (the caller uses a no-echo prompt), `NEMA_ERR_PERM` if called
  from the desktop task (use the async version), `NEMA_ERR_BUSY` if another keyboard is already open.
- Used by `wifi connect` (title "Wi-Fi password", mask, allow_empty so that open networks can go without a password).
  Without a screen, `wifi connect` uses `pal_read_password` (a no-echo line, in `apps/libapp.c`).

## Wiring
- `pal_vk_register()` is called in `apps/boot.c` after the monitor view is registered: it registers the `vk` view (flags
  `OPAQUE | NOBAR | FULLSCREEN | HIDDEN`) and creates the mailbox mutex.
- The keyboard is an overlay (`pal_desktop_overlay_push/remove`). Requests from other tasks (`vkeyboard`, `pal_vk_edit`)
  are queued in a single mailbox slot and applied in `pump` (the desktop task), so stack changes always happen on the desktop.
- Testing without a desktop: `pal_vk_set_hooks()` replaces push/pop, presence, injection, clock, and wake.

## Golden images
`forge/tools/golden-scenes.json`: `vk-abc`, `vk-upper`, `vk-123`, `vk-sym2`, `vk-ctl` (text fixed to "ls -la /apps"), and
`vk-entry-mask` (title "Wi-Fi password", sample password `Sup3r!` shown as dots). The caret stays on while the text is fixed and
right after typing, so the image is deterministic.

## Not yet verified
- The timing of Fn Fn and taps on a real board (tests use a fake clock).
- Key sizes and legibility on the real 320x240 screen, and the effect of SPI on frequently drawn keys.
- The blocking `pal_vk_edit` flow (tests verify the rejection and the no-screen path, not a real wait).
- Dark mode golden: after `theme dark` then `home-app`, the Enter key on the home screen does not open Terminal (the screen
  shows Control). The `terminal-ls-bin-dark` baseline also captures Control, so this is an existing golden flow problem.
  As a result the dark mode `vk-*` scenes have no reference yet (deliberately not made from the wrong image).
- The default mode `vk-*` screens have been viewed and used as the reference.
