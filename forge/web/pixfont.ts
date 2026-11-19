// The device's own bitmap fonts as web fonts: reads a generated font file (system/src/font_spleen_*.c, from forge/tools/bdf2c.ts)
// and writes a TrueType font whose glyphs are the lit pixels as squares. Web Forge's headings and labels then use the same
// letters as the PalanuOS screen, inlined into the page (no font server, no download). Crisp at whole multiples of the cell height.
import { readFileSync } from "node:fs";

interface Bitmap { name: string; w: number; h: number; cps: number[]; bits: number[] }

/** Parses `static const uint16_t CPS[] = {...}`, `static const uint8_t BITS[] = {...}` and the pal_font_t line. */
export function readFontC(path: string): Bitmap {
  const src = readFileSync(path, "utf8");
  const arr = (name: string) => {
    const m = new RegExp(`${name}\\[\\]\\s*=\\s*\\{([^}]*)\\}`).exec(src);
    if (!m) throw new Error(`${path}: no ${name}[]`);
    return m[1].split(",").map((s) => s.trim()).filter(Boolean).map((s) => Number(s));
  };
  const def = /pal_font_t\s+\w+\s*=\s*\{\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+)/.exec(src);
  if (!def) throw new Error(`${path}: no pal_font_t`);
  return { name: def[1], w: Number(def[2]), h: Number(def[3]), cps: arr("CPS"), bits: arr("BITS") };
}

const U = 100; /* font units per pixel */

class Buf {
  b: number[] = [];
  u8(v: number) { this.b.push(v & 255); return this; }
  u16(v: number) { return this.u8(v >> 8).u8(v); }
  i16(v: number) { return this.u16(v < 0 ? v + 65536 : v); }
  u32(v: number) { return this.u16(Math.floor(v / 65536) & 0xffff).u16(v & 0xffff); }
  tag(s: string) { for (const c of s) this.u8(c.charCodeAt(0)); return this; }
  bytes(a: number[] | Uint8Array) { for (const v of a) this.b.push(v); return this; }
  pad4() { while (this.b.length % 4) this.b.push(0); return this; }
}

function checksum(b: number[]): number {
  let sum = 0;
  for (let i = 0; i < b.length; i += 4) sum = (sum + (((b[i] ?? 0) << 24) >>> 0) + ((b[i + 1] ?? 0) << 16) + ((b[i + 2] ?? 0) << 8) + (b[i + 3] ?? 0)) >>> 0;
  return sum;
}

/** A TrueType font of the printable glyphs (U+0020 up, the Basic Multilingual Plane) of a bitmap font. */
export function toTtf(f: Bitmap, family: string, copyright: string): Uint8Array {
  const bpr = (f.w + 7) >> 3;
  const lit = (gi: number, x: number, y: number) => (f.bits[gi * f.h * bpr + y * bpr + (x >> 3)] >> (7 - (x & 7))) & 1;
  /* the baseline: one row under the lowest pixel of "H" (fonts without it: the cell's bottom eighth is the descent) */
  const hIdx = f.cps.indexOf(72);
  let base = f.h - Math.max(1, Math.round(f.h / 8));
  if (hIdx >= 0) for (let y = 0; y < f.h; y++) for (let x = 0; x < f.w; x++) if (lit(hIdx, x, y)) base = y + 1;
  const asc = base * U, desc = (f.h - base) * U;

  const use = f.cps.map((cp, i) => ({ cp, i })).filter((g) => g.cp >= 32 && g.cp <= 0xfffd);
  const glyphs: number[][] = [[]]; /* glyph 0: .notdef, empty */
  let maxPts = 0, maxCont = 0, xMin = 0, yMin = 0, xMax = 0, yMax = 0;
  for (const g of use) {
    const rects: [number, number, number, number][] = [];
    for (let y = 0; y < f.h; y++)
      for (let x = 0; x < f.w;) {
        if (!lit(g.i, x, y)) { x++; continue; }
        let e = x;
        while (e < f.w && lit(g.i, e, y)) e++;
        rects.push([x * U, (base - y - 1) * U, e * U, (base - y) * U]);
        x = e;
      }
    if (!rects.length) { glyphs.push([]); continue; }
    const gx0 = Math.min(...rects.map((r) => r[0])), gy0 = Math.min(...rects.map((r) => r[1]));
    const gx1 = Math.max(...rects.map((r) => r[2])), gy1 = Math.max(...rects.map((r) => r[3]));
    xMin = Math.min(xMin, gx0); yMin = Math.min(yMin, gy0); xMax = Math.max(xMax, gx1); yMax = Math.max(yMax, gy1);
    maxPts = Math.max(maxPts, rects.length * 4); maxCont = Math.max(maxCont, rects.length);
    const b = new Buf().i16(rects.length).i16(gx0).i16(gy0).i16(gx1).i16(gy1);
    rects.forEach((_, k) => b.u16(k * 4 + 3));
    b.u16(0); /* no instructions */
    for (let k = 0; k < rects.length * 4; k++) b.u8(1); /* on-curve, coordinates as 16-bit deltas */
    const pts: [number, number][] = [];
    for (const [x0, y0, x1, y1] of rects) pts.push([x0, y0], [x0, y1], [x1, y1], [x1, y0]); /* clockwise: an outer contour */
    let px = 0;
    for (const [x] of pts) { b.i16(x - px); px = x; }
    let py = 0;
    for (const [, y] of pts) { b.i16(y - py); py = y; }
    glyphs.push(b.pad4().b);
  }
  const n = glyphs.length, adv = f.w * U;

  const glyf = new Buf(), loca = new Buf();
  for (const g of glyphs) { loca.u32(glyf.b.length); glyf.bytes(g); }
  loca.u32(glyf.b.length);

  /* cmap format 4: one segment per code point, idDelta maps it to its glyph */
  const segs = use.map((g, k) => ({ cp: g.cp, gid: k + 1 }));
  const segX2 = (segs.length + 1) * 2;
  const sr = 2 * 2 ** Math.floor(Math.log2(segs.length + 1));
  const sub = new Buf().u16(4).u16(16 + segX2 * 4).u16(0).u16(segX2).u16(sr).u16(Math.log2(sr / 2)).u16(segX2 - sr);
  for (const s of segs) sub.u16(s.cp);
  sub.u16(0xffff).u16(0);
  for (const s of segs) sub.u16(s.cp);
  sub.u16(0xffff);
  for (const s of segs) sub.u16((s.gid - s.cp + 65536) & 0xffff);
  sub.u16(1);
  for (let k = 0; k <= segs.length; k++) sub.u16(0);
  const cmap = new Buf().u16(0).u16(1).u16(3).u16(1).u32(12).bytes(sub.b);

  const hmtx = new Buf();
  for (let k = 0; k < n; k++) hmtx.u16(adv).i16(0);
  const head = new Buf().u32(0x10000).u32(0x10000).u32(0).u32(0x5f0f3cf5).u16(0x000b).u16(f.h * U)
    .u32(0).u32(0).u32(0).u32(0).i16(xMin).i16(yMin).i16(xMax).i16(yMax).u16(0).u16(8).i16(2).i16(1).i16(0);
  const hhea = new Buf().u32(0x10000).i16(asc).i16(-desc).i16(0).u16(adv).i16(0).i16(0).i16(xMax).i16(1).i16(0).i16(0)
    .i16(0).i16(0).i16(0).i16(0).i16(0).u16(n);
  const maxp = new Buf().u32(0x10000).u16(n).u16(maxPts).u16(maxCont).u16(0).u16(0).u16(2).u16(0).u16(0).u16(0).u16(0).u16(0).u16(0).u16(0).u16(0);
  const os2 = new Buf().u16(4).i16(adv).u16(400).u16(5).u16(0)
    .i16(f.h * U / 2).i16(f.h * U / 2).i16(0).i16(desc).i16(f.h * U / 2).i16(f.h * U / 2).i16(0).i16(Math.round(asc / 2)).i16(U).i16(Math.round(asc / 3))
    .i16(0).bytes([2, 11, 6, 9, 0, 0, 0, 0, 0, 0]).u32(1).u32(0).u32(0).u32(0).tag("PALN").u16(0x40)
    .u16(Math.min(...segs.map((s) => s.cp))).u16(Math.min(0xffff, Math.max(...segs.map((s) => s.cp))))
    .i16(asc).i16(-desc).i16(0).u16(asc).u16(desc).u32(1).u32(0).i16(Math.round(asc * 0.7)).i16(asc).u16(0).u16(32).u16(1);
  const post = new Buf().u32(0x30000).u32(0).i16(-U).i16(U).u32(1).u32(0).u32(0).u32(0).u32(0);
  const names: [number, string][] = [[0, copyright], [1, family], [2, "Regular"], [3, `${family} ${f.name}`], [4, family], [6, family.replace(/\s+/g, "")]];
  const strs = names.map(([, s]) => [...s].flatMap((c) => { const v = c.charCodeAt(0); return [v >> 8, v & 255]; }));
  const name = new Buf().u16(0).u16(names.length).u16(6 + names.length * 12);
  let off = 0;
  names.forEach(([id], k) => { name.u16(3).u16(1).u16(0x409).u16(id).u16(strs[k].length).u16(off); off += strs[k].length; });
  for (const s of strs) name.bytes(s);

  const tables: [string, number[]][] = [["OS/2", os2.b], ["cmap", cmap.b], ["glyf", glyf.b], ["head", head.b], ["hhea", hhea.b],
    ["hmtx", hmtx.b], ["loca", loca.b], ["maxp", maxp.b], ["name", name.b], ["post", post.b]];
  const nt = tables.length, es = Math.floor(Math.log2(nt)), srch = 2 ** es * 16;
  const out = new Buf().u32(0x10000).u16(nt).u16(srch).u16(es).u16(nt * 16 - srch);
  let at = 12 + nt * 16;
  const placed: { off: number; len: number }[] = [];
  for (const [tag, data] of tables) {
    out.tag(tag).u32(checksum(data)).u32(at).u32(data.length);
    placed.push({ off: at, len: data.length });
    at += (data.length + 3) & ~3;
  }
  for (const [, data] of tables) out.bytes(data).pad4();
  const headAt = placed[3].off + 8;
  const adj = (0xb1b0afba - checksum(out.b)) >>> 0;
  out.b[headAt] = adj >>> 24; out.b[headAt + 1] = (adj >> 16) & 255; out.b[headAt + 2] = (adj >> 8) & 255; out.b[headAt + 3] = adj & 255;
  return Uint8Array.from(out.b);
}

/** The @font-face rule for a bitmap font file, with the TrueType font inlined. */
export function fontFace(path: string, family: string): string {
  const f = readFontC(path);
  const lic = /Copyright[^\n]*/.exec(readFileSync(path, "utf8"))?.[0] ?? "";
  const ttf = toTtf(f, family, `${lic.replace(/^\s*\*\s*/, "")}. BSD-2-Clause.`);
  return `@font-face{font-family:"${family}";src:url(data:font/ttf;base64,${Buffer.from(ttf).toString("base64")}) format("truetype");font-display:block}`;
}
