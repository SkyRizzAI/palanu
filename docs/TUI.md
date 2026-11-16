# TUI toolkit: paltui

`paltui` is a terminal interface library for PalanuOS apps (T2.11). It is compiled into the app (wasm or
native host), and talks to the TTY only through one small interface (`tui_io_t`). The result is the same on the device's
Terminal, UART, Palanu Link, and SSH, because all of those paths are a TTY with the same ANSI output.

Files: `sdk/paltui/paltui.h` (API and the implementation opener), `sdk/paltui/paltui_core.inc` (buffer, output,
input decoder, frame, default backend), `sdk/paltui/paltui_widgets.inc` (layout and widgets),
`sdk/paltui/paltui_modal.inc` (prompt, confirm, picker). `paltui.c` only includes the header to
be used as a separate unit. Example app: `sdk/examples/menu.c`. Host test: `tests/unit/test_tui.c`.

## Model

Immediate mode, like microui and termbox2:

1. `tui_frame(&ui, timeout)` waits for input, a size change, or stop. Then the back buffer is cleared.
2. The app runs its frame code, calling widgets. Widgets write cells to the back buffer.
3. `tui_end(&ui)` compares the back buffer with the screen already shown, then sends only the cells that changed
   (the cursor is moved if needed, SGR is sent if the style changed). After that the buffers are swapped.

The same frame twice sends no bytes. Moving one selection row on a 64-column menu sends about
50 bytes in 256-color mode (measured in `test_tui`).

One key is processed per frame. Input that piles up in a single read (e.g. fast typing) is processed frame by
frame, without loss.

## Usage: an app in 30 lines

Build: `bun forge/tools/palanu.ts build app.c --id hello --perm fs` (palanu adds `-I sdk`).

```c
#define PALTUI_IMPLEMENTATION
#define PALTUI_WITH_PAL
#include "paltui/paltui.h"

static tui_t ui;
static const char *const items[] = {"Terminal", "Files", "Quit"};

void _start(void) {
    char name[32] = "";
    int sel = 0;
    if (tui_open(&ui, tui_io_pal()) != 0) return;
    while (tui_frame(&ui, 1000) == TUI_RUN) {
        int cols, rows;
        tui_size(&ui, &cols, &rows);
        tui_region(&ui, 2, 1, cols - 4, rows - 2);
        tui_title(&ui, "Hello");
        tui_gap(&ui, 1);
        int chose = tui_menu(&ui, "menu", items, 3, &sel);
        tui_gap(&ui, 1);
        tui_input(&ui, "name", name, sizeof(name));
        tui_status(&ui, " Enter choose   Esc quit ");
        const tui_key_t *k = tui_key(&ui); /* keys no widget took */
        if (k && k->sym == TUI_K_ESC) break;
        tui_end(&ui);
        if (chose && sel == 2) break;
    }
    tui_close(&ui);
}
```

Rules to remember:
- In every frame, widgets are called in the same order. The widget id (a string) determines focus and state.
- Tab and Shift-Tab are used by the library to move focus. Other keys are taken by the focused widget, or by
  `tui_key()` if nothing took them.
- Modals (`tui_prompt`, `tui_confirm`, `tui_pick`) are called between frames, that is, after `tui_end`. A modal
  draws over the last screen, and the next frame redraws.

## Widgets

| Function | Result | Notes |
|---|---|---|
| `tui_title(t, s)` | | One line, TITLE style |
| `tui_label(t, s)` | | One line, truncated |
| `tui_text(t, s)` | | Word-wrapped, several lines |
| `tui_menu(t, id, items, n, &sel)` | 1 on Enter | Arrows, Home/End, PgUp/PgDn; the selected row is marked `> ` |
| `tui_list(t, id, items, n, &sel)` | 1 if the selection moved | Same as menu, without activation |
| `tui_input(t, id, buf, cap)` | bit 1: text changed, bit 2: Enter | Line editor: cursor, insert, delete, UTF-8; Ctrl-A/E/U |
| `tui_password(t, id, buf, cap)` | same | Shows `*`, the buffer still holds the original text |
| `tui_button(t, id, label)` | 1 if pressed | Enter or Space |
| `tui_checkbox(t, id, label, &on)` | 1 if changed | Enter or Space |
| `tui_progress(t, v, max)` | | A bar `[###---] 42%` |
| `tui_box(t, x, y, w, h, title)` | | A frame at absolute coordinates |
| `tui_status(t, s)` | | Bottom row, SEL style |
| `tui_prompt(t, title, buf, cap)` | 1 Enter, 0 Esc | Modal, a single field |
| `tui_confirm(t, text)` | 1 yes, 0 no | Modal; Y, N, Enter, Esc, left/right arrows |
| `tui_pick(t, title, items, n)` | index, or -1 | Modal, a list |

Layout: `tui_region(t, x, y, w, h)` selects an area. Widgets are stacked downward from row `cy`. `tui_gap(t, n)`
skips rows. There is no row/column layout (`tui_row`/`tui_col` were not made; keep using regions).

## Input

TTY bytes are decoded into `tui_key_t { sym, cp, mods }`, the same structure that GUI apps will use later:

- Characters: `sym = 0`, `cp` holds the UTF-8 code point. Ctrl+letter: `cp` is the lowercase letter and `TUI_MOD_CTRL`.
- Alt+letter: an ESC prefix, `TUI_MOD_ALT`.
- `TUI_K_ENTER`, `TAB`, `BACKSPACE`, `DELETE`, `ESC`, arrows, `HOME`, `END`, `PGUP`, `PGDN`, `INSERT`, `F1`..`F12`.
  Modifiers from the CSI parameter (Shift, Alt, Ctrl) go into `mods`.
- A lone ESC is recognized after 40 ms without a following byte.
- Bracketed paste (`ESC[200~` .. `ESC[201~`) is delivered as `tui_paste()` chunks, which end at a character
  boundary. Input widgets take only printable characters.

## Backend

```c
typedef struct {
    int32_t (*write)(const void *buf, uint32_t len);
    int32_t (*read)(void *buf, uint32_t cap, int32_t timeout_ms); /* bytes, 0 timeout, TUI_IO_WAKE_* */
    int32_t (*size)(int *cols, int *rows);
    int32_t (*caps)(void);   /* TUI_CAP_256 | TUI_CAP_16 | TUI_CAP_UTF8 | TUI_CAP_ALT, or 0 for mono */
    int32_t (*now_ms)(void);
} tui_io_t;
```

- `tui_io_pal()` (with `PALTUI_WITH_PAL`): `pal_write(1, ...)`; `pal_wait` on stdin and `pal_events()`;
  `PAL_EV_RESIZE` becomes `TUI_IO_WAKE_RESIZE`; `PAL_EV_QUIT` or HUP becomes `TUI_IO_WAKE_QUIT`; RAW mode on
  `tui_open`, restored to the original mode on `tui_close` or `tui_suspend`.
- `tui_wait_extra(t, h, want)` adds other handles (files, pipes, sockets) that `tui_frame` also waits on. When one is
  ready, `tui_frame` returns without a key and `tui_extra_got(h)` gives the ready bits. The app reads that handle
  itself. Only available with `PALTUI_WITH_PAL`.
- Tests use a fake backend: timestamped input, a fake clock, a fake size, and collected output.

Semantic styles are mapped according to caps: 256 colors, 16 colors, or mono (bold, reverse, underline). On mono,
the menu selection is shown with reverse. Alternate screen and a hidden cursor are used on `tui_open`,
and both are restored on `tui_close`. `tui_suspend` / `tui_resume` restore the main screen and cursor so that an
SSH session can take the TTY, then redraw the whole screen on `tui_resume`.

## Memory and size

There is no heap. `tui_t` is created statically by the app.

| Part | Bytes (default) |
|---|---|
| `tui_t` (two 100x40 cell buffers x 4 bytes, plus output, input, slots) | 33,864 |
| Cell buffers only | 32,000 |
| Output `PALTUI_OUT_BYTES` | 1,024 |
| State slots per widget `PALTUI_SLOTS` (16) | 256 |

Macros that can be changed before the include: `PALTUI_MAX_COLS` (100), `PALTUI_MAX_ROWS` (40), `PALTUI_OUT_BYTES`,
`PALTUI_MAX_IDS` (32, focus per frame), `PALTUI_SLOTS` (16), `PALTUI_EXTRA` (4).

Wasm size (measured with clang `--target=wasm32`, rust-lld):

- `sdk/examples/menu.c` with `-O2` (palanu flag): 26,835 bytes. `.papp` package: 26,870 bytes.
- The same with `-Os`: 15,727 bytes. Palanu currently uses `-O2`; switching to `-Os` shrinks this app by almost
  half (a decision outside T2.11).

Frame cost for a 64x30 screen with all rows filled (`worst_case_frame_cost`): 1,920 cell writes by
widgets per frame. Frames and the status bar are not counted.

## Limits

- Character width: 1 column, or 2 for CJK/Hangul/full-width blocks. Code points outside the BMP are shown as `?`.
  Combining characters count as 1 column.
- The terminal emulator on the device (`system/src/term.c`) does not know width 2 yet: every code point takes 1 column.
  So on the device, CJK text can be misaligned. The library sends width-2 pairs correctly. The fix belongs
  in the emulator, not in the library.
- One key per frame; no mouse.
- There is no automatic row/column layout; use `tui_region`.
- A modal cannot be called in the middle of a frame (before `tui_end`): it draws over the last screen.
- The screen is limited to `PALTUI_MAX_COLS` x `PALTUI_MAX_ROWS`; anything larger is cut.
- The app's output to stdout must not mix with an open TUI (use `tui_suspend` first).
