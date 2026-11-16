#!/usr/bin/env python3
"""Rasterise Symbols Nerd Font Mono to 8x16 one-cell glyphs in Unifont .hex form (CODEPOINT:32 hex digits), for fontpack.ts.

    python3 forge/tools/nerd2hex.py SymbolsNerdFontMono-Regular.ttf assets/fonts/nerd/nerdsymbols.hex

Needs Pillow and fontTools (a throwaway venv is enough); the output is committed, so building the firmware needs neither.
Each icon is drawn at 64 px, its ink box is fitted into 8 x 14 pixels keeping the aspect ratio and never enlarged past 16 px
em (a dot stays a dot), centred in the 8x16 cell, and averaged down: a pixel is set when 40% of it is covered. The Powerline
Extra shapes (U+E0C0-E0D7) fill the whole cell instead, as the separators do.
"""
import sys
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

src, out = sys.argv[1], sys.argv[2]
cmap = TTFont(src).getBestCmap()
big = ImageFont.truetype(src, 64)
CW, CH = 8, 16


def cell(cp):
    img = Image.new("L", (128, 128), 0)
    ImageDraw.Draw(img).text((32, 32), chr(cp), font=big, fill=255)
    box = img.getbbox()
    if not box:
        return None
    ink = img.crop(box)
    w, h = ink.size
    if 0xE0C0 <= cp <= 0xE0D7:
        tw, th = CW, CH
    else:
        k = min(CW / w, 14 / h, 0.25)  # 0.25: 64 px em to 16 px em
        tw, th = max(1, round(w * k)), max(1, round(h * k))
    small = ink.resize((tw, th), Image.BOX)
    canvas = Image.new("L", (CW, CH), 0)
    canvas.paste(small, ((CW - tw) // 2, (CH - th) // 2))
    rows = []
    for y in range(CH):
        b = 0
        for x in range(CW):
            if canvas.getpixel((x, y)) >= 102:
                b |= 0x80 >> x
        rows.append(b)
    return rows if any(rows) else None


n = 0
with open(out, "w") as f:
    for cp in sorted(cmap):
        if cp < 0x2000 or 0xE0A0 <= cp <= 0xE0BF:
            continue
        rows = cell(cp)
        if rows is None:
            continue
        f.write(f"{cp:04X}:{''.join(f'{b:02X}' for b in rows)}\n")
        n += 1
print(f"{out}: {n} glyphs")
