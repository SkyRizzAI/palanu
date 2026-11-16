# Animations (.panim)

This document describes the PalanuOS animation format, its player, and how the built-in animations are made. The code lives in
`system/src/anim.c` (reader), `system/src/anim_draw.c` (player and drawing), `system/include/pal/anim.h` (API),
`forge/tools/panim.ts` (generator), and `assets/anims/` (sources).

## Format

PalanuOS reads `.panim` version 1 and version 2. The tests check the format in both directions: `forge/tools/panim.test.ts` uses
fixed byte fixtures and checks that our encoder produces the same bytes; `tests/unit/test_anim.c` loads the same fixtures in C.

| Part | Version 1 | Version 2 |
| --- | --- | --- |
| Header | 14 bytes: `PANM`, version, width u16, height u16, fps, passive, active, bitmap count, sequence length | 16 bytes: v1 header, then `pixelFormat` (0 mono, 1 indexed4) and `extLen` |
| Extension | - | `extLen` bytes of TLV, skipped (the reader ignores unknown entries) |
| Palette | - | indexed4 only: `count` (1 to 15), then per entry `rgb565` (LE) and `monoOverride` |
| Sequence | `framesOrder[]` | same |
| Bitmap | mono1: `ceil(w/8)*h` bytes, bit `row*w+col` (contiguous), MSB first | mono1: same; indexed4: `ceil(w/2)*h` bytes, byte-aligned rows, high nibble = even x |

Indexed4 nibble values: 0 is transparent, `n` means `palette[n-1]`. A value above the palette count is treated as transparent.

Limits enforced by the reader: the file is at most 512 KB (`PAL_ANIM_MAX_BYTES`), width and height are at most 320x240,
`fps` is not zero, `framesOrder` only points to bitmaps that exist, and passive and active do not exceed the number of positions.
A truncated file, one with the wrong magic, or one with another version is rejected before anything is used.

**There is no version 3.** Color needs are met by v2 (indexed4 with a palette). The player maps each palette entry to a theme
slot, so a new version is not needed. The mapping rules are below.

### Mapping the palette to theme slots

`pal_anim_slot(rgb565, monoOverride)` (`anim_draw.c`) is evaluated at draw time, so dark mode and other themes apply:

- `monoOverride` 1: ink (`PAL_C_INK`). 2: paper (`PAL_C_PAPER`).
- `monoOverride` 0 (automatic): a saturated color (max-min difference of 96 or more) becomes accent (`PAL_C_ACCENT`). Otherwise
  luminance below 96 becomes ink, below 176 becomes dim, below 232 becomes panel, and the rest becomes paper.

Mono1 (v1 and v2 mono): a set bit uses the caller's `on` color (usually `PAL_C_INK`), a clear bit uses `off`
(can be `PAL_TRANSPARENT`).

## Player

The player has no thread. The caller (a view) supplies time in milliseconds on every tick:

- Positions are `0..positions-1`. Positions `0..passive-1` are the passive loop. If `passive` is zero, the whole sequence is the loop.
- `pal_anim_play_trigger` runs the active sequence (`passive..passive+active-1`) once, then returns to the start of the passive loop.
  Without an active sequence, the trigger does nothing.
- One position = `1000 / fps` ms. Elapsed time is converted into several steps at once (limited to the number of positions).
  The player keeps the remainder of the time, so steps are not lost. The 32-bit counter wraps correctly.
- `pal_anim_play_tick` returns `true` only if the bitmap on screen changed. The caller redraws only that area.
- `pal_anim_play_seek` moves the position (used by golden images).

An animation is opened with `pal_anim_open_builtin` (read straight from flash, only the struct goes into RAM) or
`pal_anim_open_file` (the file `/data/anims/NAME.panim` is copied into a single PSRAM block owned by the `anim` owner). Block size
= file bytes + struct; `pal_anim_memory` reports it and `anim info` shows it. Names are lowercase letters, digits, and `-` only.

## Drawing

- `pal_anim_draw(c, a, bm, x, y, scale, on, off)`: integer scale 1, 2, or 3, nearest neighbor, no interpolation.
- `pal_anim_draw_fit(c, a, bm, area, fit, anchor, on, off)`: fit into a rectangle. Fit modes: `center` (original size, placed at
  the anchor), `stretch` (fills, aspect ratio ignored), `crop` (covers, overflow is clipped), `fit` (fits inside, the rest is left
  alone). The nine anchors in reading order are: top-left, top, top-right, left, center, right, bottom-left, bottom,
  bottom-right.
- `pal_anim_fit_rect` is the pure part: the fitted rectangle without drawing. Used by tests and the wallpaper.
- Every draw call marks (`pal_mark`) only the rectangle it drew. With a colored `off`, the area is filled first and the whole
  area is marked. The screen sends only that dirty area.

## Built-in animations

The six original PalanuOS animations are in `assets/anims/<name>/`. All of them are original drawings made for this project; see
`docs/SOURCES.md` if applicable. Size is 128x64, built from basic shapes and ASCII sprites.

| Name | Unique frames | Positions | fps | Active sequence | Content |
| --- | --- | --- | --- | --- | --- |
| `coder` | 16 | 16 | 8 | none | A programmer in front of a laptop; screen text scrolls, hands alternate, the cursor blinks |
| `sleepy-cat` | 17 | 24 (16 passive, 8 active) | 5 | stretch (8 frames) | A cat curled up on a cushion, breathing, tail moving now and then, Zzz rising |
| `server` | 8 | 8 | 8 | none | A server rack with blinking LEDs and a spinning fan |
| `rain` | 12 | 12 | 6 | none | A skyline with lit windows and rain; a puddle with ripples |
| `rocket` | 15 | 16 (6 passive, 10 active) | 8 | launch (10 frames) | A rocket sitting on the pad with a flickering flame; launch and stars |
| `coffee` | 8 | 8 | 6 | none | A steaming cup on a saucer, in front of a keyboard with moving keys |

File sizes (`assets/anims/out/*.panim`): coder 16 KB, sleepy-cat 17 KB, server 32 KB, rain 49 KB, rocket 61 KB,
coffee 32 KB. Server, rain, rocket, and coffee use the accent (`+`), so they are v2 indexed4 (4 bits per pixel); the others are
v1 mono.

## Sources: `assets/anims/<name>/`

`forge/tools/panim.ts` turns each directory into a `.panim` (`assets/anims/out/`) and into C tables
(`system/src/anims_data.c`, `system/include/pal/anims_data.h`, and per-animation bytes in `system/src/anims/<name>.inc`).
`.inc` files contain data only, so the line-length rule of `./pal check` (800/1000 lines) applies only to code.
`bun forge/tools/panim.ts --check` fails if the output does not match the sources; `./pal check` runs it.

Directory contents:

- `anim.json`: the spec (see below).
- `base.txt` (optional): the same 128x64 frame under all frames; `#` ink, `+` accent, `.` empty.
- `<sprite>.txt`: sprites, rows of equal length. `#` ink, `+` accent, `.` transparent.

`anim.json`:

```json
{
  "fps": 8,
  "size": [128, 64],
  "accent": "#ff6a2a",
  "frames": 16,
  "idle": [0, 16],
  "active": [16, 24],
  "hold": [4, 1, 1],
  "shapes": [ {"box": [34, 2, 60, 36]}, {"dots": [31, 39, 64, 2], "every": 2}, {"disc": [17, 25, 6]} ],
  "tracks": [
    {"sprite": "code", "x": 40, "y": {"base": 0, "step": -2, "wrap": 32, "lo": 8}, "clip": [38, 8, 52, 26]},
    {"sprite": "caret", "x": 80, "y": 27, "show": [1, 1, 0, 0]},
    {"sprite": ["fan-0", "fan-1", "fan-2", "fan-3"], "x": 88, "y": 22}
  ]
}
```

- `frames`: the number of frames on the timeline (1 to 255). `idle` starts at 0; `active` starts right after `idle` and ends at
  `frames`. A frame that belongs to neither is unused, so it is forbidden.
- `hold`: how many positions each frame is held (default 1).
- `shapes`: basic shapes drawn once: `fill`, `box`, `line`, `disc`, `ring`, `dots`. Optional `c`: `#`, `+`, or
  `.` to erase.
- `tracks`: sprites drawn on top of the shapes, in order. `x` and `y` can be a number, a per-frame array, or
  `{base, step, wrap, lo}`, which evaluates to `lo + ((base + step * t) mod wrap)`. A sprite can be an array (switching sprite per frame).
  `show` (0 or 1, or an array) hides it. `clip` clips to a rectangle.
- The built-in animations are regenerated whenever the sources change. Add an animation by creating a new directory and running the generator.
  Names must be lowercase letters, digits, and `-`.

## Settings and the screens that use them

| Key | Values | Default | Used in |
| --- | --- | --- | --- |
| `ui.wallpaper` | `none`, `stars`, `city`, `waves`, `anim:NAME` | `stars` | home. The animation is filled in as the wallpaper |
| `ui.wall_fit` | `center`, `fit`, `stretch`, `crop` | `fit` | home, the mode for animations |
| `ui.wall_anchor` | nine anchors, for example `bottom` | `bottom` | home, the anchor for animations |
| `ui.lock_anim` | `none` or NAME | `sleepy-cat` | lock screen, 1x next to the clock |

A lock screen with an animation uses a different layout: the animation at 1x on the left, the clock on the right, and LOCKED, the
hint, and the notification summary below. With `none` the layout is the same as before animations existed. The animation stops when
the screen sleeps (DPM) or when an overlay covers the screen. On home, only the animation area is redrawn; the clock gets a paper
plate so it stays readable.

The `anim` command (dev, `apps/cmd_anim.c`): `anim list`, `anim info NAME`, `anim play NAME [1-3]` (Enter triggers the active
sequence, Esc closes), `anim sheet NAME [page]` (frame sheet, six per page), `anim bench NAME [scale]`.

Splash (`system/src/ui_splash.c`): at boot, the desktop is opened with the splash as the front view. The splash shows the PalanuOS
mark, a moon orbiting its ring, the name, and a progress bar from the boot stages (`pal_boot_mark`). The splash ends after
all stages are recorded and at least 1.5 seconds (at most 5 seconds), then opens home. Keys are ignored during the splash.
Restart (the Restart button in Control and the `reboot` command) uses the same splash with the text "Restarting".

## Measurements

Measured on the host simulator (Debug build, no optimization), with `anim bench NAME [scale]` (average over several rounds):

| Image | Time per frame (host) | Area sent |
| --- | --- | --- |
| 2x (256x128) | 147 to 250 us | 256x128 px, 65,536 bytes |
| fit to 320x240 (home) | 243 to 286 us | 320x160 px, 102,400 bytes |

The hhv2 LCD uses SPI at 80 MHz with 16 bits per pixel (`boards/hhv2/hhv2_hw.c`). Estimated bus time per frame from those
numbers, without DMA overhead: 256x128 about 6.6 ms; 320x160 about 10 ms; 128x64 (lock 1x) about 1.6 ms. At 8 fps that is
about 5 to 8 percent of bus time. **Not yet measured on the board**: draw time on the ESP32 and the cost of converting the palette to RGB565
on screen must be measured with `perf` after flashing (orchestrator).

Memory: each animation opened from a file uses PSRAM equal to the file plus the struct (about 170 bytes). Built-in animations
use only the struct in RAM; their bitmaps stay in flash (`anim info` shows both).

Source and output sizes are given above. The total bytes of the built-in animations (`assets/anims/out`) are about 228 KB; the generated
C text (`system/src/anims/*.inc`) is about 1.3 MB, because every byte is written as `0x..`.
