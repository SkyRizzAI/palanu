# Terminal glyphs: where they come from

The terminal draws each character with this order, stopping at the first one that can:

1. **Drawn by itself to fit the cell size** (`system/src/term_glyph.c`): box drawing and block elements (U+2500-259F), Braille
   (U+2800-28FF, for spinners and graphs), Powerline separators (U+E0B0-E0BF) and its four marks (U+E0A0-E0A3), the middle dot,
   bullet, squares, triangles, arrows, the check mark. Equally good in the Small, Medium, and Large fonts.
2. **Spleen font** 5x8, 6x12, 8x16 (BSD-2-Clause): Latin, Greek, Cyrillic, punctuation, technical symbols.
3. **Fallback font** (`system/src/font_fallback.c`): everything else, 8x16 or 16x16, scaled to the cell (a destination pixel is on
   if a third of its area is on in the source, so thin strokes survive in small fonts). It contains:
   - GNU Unifont 17.0.05 (OFL 1.1): the whole BMP except CJK ideographs and Hangul (Hebrew, Arabic, Thai, Devanagari, Armenian, math
     symbols, kana, Bopomofo ...), plus emoji outlines (U+1F000-1FAFF);
   - Symbols Nerd Font Mono icons (MIT, the icons inside are individually licensed): Font Awesome, Octicons, Codicons, Seti-UI,
     Devicons, Weather, Font Logos, Material Design (U+F0001 and up), Powerline Extra shapes.
   This part is **embedded in the firmware image** (714 KB, read-only, mapped from flash: uses no RAM). Because Unifont
   draws Indic scripts and emoji 16 pixels wide while the terminal gives one column (Indic) or two (emoji), wide glyphs are
   compressed into their columns.
4. **CJK file** `/data/unifont-cjk.pfnt` (1.2 MB): CJK ideographs and Hangul syllables. Too big for the image (the OTA slot is
   4 MB), so it is shipped as a file and read per glyph; only its run table is in RAM. Without this file those characters
   appear as empty boxes. Install it from a computer:

   ```
   palanu --serial /dev/cu.usbserial-10 put unifont-cjk.pfnt /data/unifont-cjk.pfnt
   ```

   `./pal release` puts `unifont-cjk.pfnt` in `dist/release/`. The terminal looks for the file again every 10 seconds if it is not there yet.
5. **An empty box** for characters nobody has, not a question mark that could be mistaken for a real character.

What it does not do: *shaping* (Arabic stays in isolated forms, Thai and Devanagari combining marks are not stacked) and color emoji
(emoji are drawn in one color). That is the limit of a fixed-cell terminal, the same as terminals without a text engine.

## Regenerating the data

Sources are in `assets/fonts/unifont/` (Unifont, `LICENSE.txt`) and `assets/fonts/nerd/` (`nerdsymbols.hex`, output of `nerd2hex.py`, `LICENSE`).

```
bun forge/tools/fontpack.ts embedded system/src/font_fallback_data.c assets/fonts/unifont/LICENSE.txt \
    assets/fonts/unifont/unifont-17.0.05.hex.gz assets/fonts/unifont/unifont_upper-17.0.05.hex.gz assets/fonts/nerd/nerdsymbols.hex
bun forge/tools/fontpack.ts file unifont-cjk.pfnt assets/fonts/unifont/unifont-17.0.05.hex.gz assets/fonts/unifont/unifont_upper-17.0.05.hex.gz
python3 forge/tools/nerd2hex.py SymbolsNerdFontMono-Regular.ttf assets/fonts/nerd/nerdsymbols.hex     # needs Pillow and fontTools
```

The file format (`PFN1`, code-numbered runs, 16 rows per glyph) is in the header of `forge/tools/fontpack.ts`. Cost in the image: before
the fallback glyphs 66% of the OTA slot, afterwards 84% (the release gate warns at 85% and fails at 95%), so any further
additions must go to the file, not the image.
