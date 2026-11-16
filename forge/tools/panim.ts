#!/usr/bin/env bun
// PalanuOS animation tool: turns the ASCII sources in assets/anims/<name>/ into .panim files (assets/anims/out/<name>.panim)
// and into checked-in C tables (system/src/anims_data.c and system/include/pal/anims_data.h). No dependencies.
//
//   bun forge/tools/panim.ts           write the generated files
//   bun forge/tools/panim.ts --check   fail if the checked-in files are not what the sources produce (./pal check runs this)
//
// The format is docs/ANIM.md: .panim v1 (1-bit frames) and v2 (4-bit indexed frames with a palette). This file writes the
// lowest version that can hold the content, and reads both. The byte layouts are the ones of the reference codec, so a file made
// by either tool plays in the other.
//
// A source directory holds anim.json, base.txt (optional, a static frame under every frame) and one <sprite>.txt per sprite.
// Sprite text: rows of '#' (ink), '+' (accent, v2 only), '.' (transparent), all rows the same length.
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "../..");
const SRC_DIR = join(ROOT, "assets/anims");
const OUT_DIR = join(ROOT, "assets/anims/out");
const OUT_C = join(ROOT, "system/src/anims_data.c");
const OUT_H = join(ROOT, "system/include/pal/anims_data.h");

export const PANIM_MAGIC = [0x50, 0x41, 0x4e, 0x4d]; // "PANM"
export const PANIM_MAX_FRAMES = 255;
export const PANIM_MAX_POSITIONS = 255;

export class PanimError extends Error {}

/* ---------------------------------------------------------------- the decoded form */

export interface PanimPaletteEntry {
  rgb565: number;
  monoOverride: 0 | 1 | 2; // 0 auto (luminance and saturation), 1 force ink, 2 force paper
}

export interface Panim {
  width: number;
  height: number;
  frameRate: number;
  passiveCount: number;
  activeCount: number;
  framesOrder: number[]; // playhead -> bitmap index; empty = sequential
  frames: Uint8Array[]; // mono1: ceil(w/8)*h bytes; indexed4: ceil(w/2)*h bytes
  pixelFormat?: "mono1" | "indexed4";
  palette?: PanimPaletteEntry[];
}

export function frameBytes(w: number, h: number): number {
  return Math.ceil(w / 8) * h;
}

export function indexedFrameBytes(w: number, h: number): number {
  return Math.ceil(w / 2) * h;
}

/* ---------------------------------------------------------------- validation (shared by both writers) */

function validate(a: Panim): void {
  const { width: w, height: h, frameRate: fps, passiveCount: pc, activeCount: ac, framesOrder: order, frames } = a;
  if (!Number.isInteger(w) || w <= 0 || w > 0xffff) throw new PanimError(`bad width ${w}`);
  if (!Number.isInteger(h) || h <= 0 || h > 0xffff) throw new PanimError(`bad height ${h}`);
  if (!Number.isInteger(fps) || fps <= 0 || fps > 255) throw new PanimError(`bad frame rate ${fps}`);
  if (frames.length === 0 || frames.length > PANIM_MAX_FRAMES) throw new PanimError(`${frames.length} unique frames, limit ${PANIM_MAX_FRAMES}`);
  if (pc < 0 || pc > 255 || ac < 0 || ac > 255) throw new PanimError(`passive/active count out of range`);
  if (order.length > PANIM_MAX_POSITIONS) throw new PanimError(`framesOrder has ${order.length} entries, limit ${PANIM_MAX_POSITIONS}`);
  if (a.pixelFormat === "indexed4") {
    const pal = a.palette ?? [];
    if (pal.length < 1 || pal.length > 15) throw new PanimError(`indexed4 palette needs 1..15 entries, has ${pal.length}`);
    pal.forEach((p, i) => {
      if (!Number.isInteger(p.rgb565) || p.rgb565 < 0 || p.rgb565 > 0xffff) throw new PanimError(`palette[${i}]: bad rgb565`);
      if (p.monoOverride !== 0 && p.monoOverride !== 1 && p.monoOverride !== 2) throw new PanimError(`palette[${i}]: bad monoOverride`);
    });
  } else if (a.palette && a.palette.length) {
    throw new PanimError("palette given for a mono1 animation");
  }
  const bpf = a.pixelFormat === "indexed4" ? indexedFrameBytes(w, h) : frameBytes(w, h);
  frames.forEach((f, i) => {
    if (f.length !== bpf) throw new PanimError(`frame ${i} is ${f.length} bytes, expected ${bpf}`);
  });
  order.forEach((v, i) => {
    if (!Number.isInteger(v) || v < 0 || v >= frames.length) throw new PanimError(`framesOrder[${i}] = ${v} is out of range`);
  });
}

/* ---------------------------------------------------------------- writers */

function header(buf: Uint8Array, dv: DataView, a: Panim, version: number): void {
  buf.set(PANIM_MAGIC, 0);
  buf[4] = version;
  dv.setUint16(5, a.width, true);
  dv.setUint16(7, a.height, true);
  buf[9] = a.frameRate;
  buf[10] = a.passiveCount;
  buf[11] = a.activeCount;
  buf[12] = a.frames.length;
  buf[13] = a.framesOrder.length;
}

export function encodeV1(a: Panim): Uint8Array {
  if (a.pixelFormat === "indexed4") throw new PanimError("v1 cannot hold indexed4 colour: use v2");
  validate(a);
  const bpf = frameBytes(a.width, a.height);
  const buf = new Uint8Array(14 + a.framesOrder.length + a.frames.length * bpf);
  const dv = new DataView(buf.buffer);
  header(buf, dv, a, 1);
  let off = 14;
  for (const v of a.framesOrder) buf[off++] = v;
  for (const f of a.frames) {
    buf.set(f, off);
    off += bpf;
  }
  return buf;
}

export function encodeV2(a: Panim): Uint8Array {
  validate(a);
  const indexed = a.pixelFormat === "indexed4";
  const bpf = indexed ? indexedFrameBytes(a.width, a.height) : frameBytes(a.width, a.height);
  const palBytes = indexed ? 1 + a.palette!.length * 3 : 0;
  const buf = new Uint8Array(16 + palBytes + a.framesOrder.length + a.frames.length * bpf);
  const dv = new DataView(buf.buffer);
  header(buf, dv, a, 2);
  buf[14] = indexed ? 1 : 0;
  buf[15] = 0; // no TLV extensions yet; readers skip them
  let off = 16;
  if (indexed) {
    buf[off++] = a.palette!.length;
    for (const p of a.palette!) {
      dv.setUint16(off, p.rgb565, true);
      off += 2;
      buf[off++] = p.monoOverride;
    }
  }
  for (const v of a.framesOrder) buf[off++] = v;
  for (const f of a.frames) {
    buf.set(f, off);
    off += bpf;
  }
  return buf;
}

/** The lowest version that can hold the animation: mono1 is v1, indexed4 is v2. */
export function encodePanim(a: Panim): Uint8Array {
  return a.pixelFormat === "indexed4" ? encodeV2(a) : encodeV1(a);
}

/* ---------------------------------------------------------------- reader (mirror of the C loader in system/src/anim.c) */

export function decodePanim(bytes: Uint8Array): Panim {
  if (bytes.length < 5) throw new PanimError("file too short");
  for (let i = 0; i < 4; i++) if (bytes[i] !== PANIM_MAGIC[i]) throw new PanimError("bad magic: not a .panim file");
  const version = bytes[4]!;
  if (version !== 1 && version !== 2) throw new PanimError(`unsupported .panim version ${version}`);
  const need = version === 1 ? 14 : 16;
  if (bytes.length < need) throw new PanimError("truncated header");
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const width = dv.getUint16(5, true);
  const height = dv.getUint16(7, true);
  const frameRate = bytes[9]!;
  const passiveCount = bytes[10]!;
  const activeCount = bytes[11]!;
  const uniqueCount = bytes[12]!;
  const orderLen = bytes[13]!;
  if (width === 0 || height === 0 || uniqueCount === 0) throw new PanimError("zero width, height or frame count");
  let off = need;
  let pixelFormat: "mono1" | "indexed4" = "mono1";
  let palette: PanimPaletteEntry[] | undefined;
  if (version === 2) {
    const pf = bytes[14]!;
    if (pf > 1) throw new PanimError(`unknown pixelFormat ${pf}`);
    const ext = bytes[15]!;
    if (off + ext > bytes.length) throw new PanimError("truncated extension area");
    off += ext;
    if (pf === 1) {
      pixelFormat = "indexed4";
      if (off + 1 > bytes.length) throw new PanimError("truncated palette");
      const count = bytes[off++]!;
      if (count < 1 || count > 15) throw new PanimError(`bad palette count ${count}`);
      if (off + count * 3 > bytes.length) throw new PanimError("truncated palette");
      palette = [];
      for (let i = 0; i < count; i++) {
        const rgb565 = dv.getUint16(off, true);
        const mo = bytes[off + 2]!;
        off += 3;
        if (mo > 2) throw new PanimError(`palette[${i}]: bad monoOverride ${mo}`);
        palette.push({ rgb565, monoOverride: mo as 0 | 1 | 2 });
      }
    }
  }
  if (off + orderLen > bytes.length) throw new PanimError("truncated framesOrder");
  const framesOrder = Array.from(bytes.subarray(off, off + orderLen));
  off += orderLen;
  const bpf = pixelFormat === "indexed4" ? indexedFrameBytes(width, height) : frameBytes(width, height);
  if (off + uniqueCount * bpf > bytes.length) throw new PanimError("truncated frame data");
  const frames: Uint8Array[] = [];
  for (let i = 0; i < uniqueCount; i++) {
    frames.push(bytes.slice(off, off + bpf));
    off += bpf;
  }
  for (const v of framesOrder) if (v >= uniqueCount) throw new PanimError(`framesOrder entry ${v} out of range`);
  const out: Panim = { width, height, frameRate, passiveCount, activeCount, framesOrder, frames };
  if (version === 2) {
    out.pixelFormat = pixelFormat;
    if (palette) out.palette = palette;
  }
  return out;
}

/* ---------------------------------------------------------------- sources: anim.json, sprites, tracks */

export type Track = {
  sprite: string | string[];
  x: Num;
  y: Num;
  show?: number | number[];
  clip?: [number, number, number, number];
};
/** A number, one per frame, or a wrap: lo + ((base + step * t) mod wrap). */
export type Num = number | number[] | { base: number; step: number; wrap: number; lo?: number };

/** A base shape (anim.json "shapes"), drawn once under every frame. c: "#" ink (default), "+" accent, "." erase. */
export type Shape =
  | { fill: [number, number, number, number]; c?: string }
  | { box: [number, number, number, number]; c?: string }
  | { line: [number, number, number, number]; c?: string }
  | { disc: [number, number, number]; c?: string }
  | { ring: [number, number, number]; c?: string }
  | { dots: [number, number, number, number]; every?: number; c?: string };

export interface Spec {
  fps: number;
  size: [number, number];
  accent?: string;
  frames: number;
  idle: [number, number];
  active?: [number, number];
  hold?: number[];
  shapes?: Shape[];
  tracks: Track[];
}

/* One sprite: rows of 0 (transparent), 1 (ink) and 2 (accent). */
export interface Sprite {
  w: number;
  h: number;
  px: Uint8Array;
}

export function parseSprite(text: string, name: string): Sprite {
  const rows = text.split("\n").filter((r) => r.length > 0);
  if (rows.length === 0) throw new PanimError(`${name}: empty sprite`);
  const w = rows[0]!.length;
  const px = new Uint8Array(w * rows.length);
  rows.forEach((r, y) => {
    if (r.length !== w) throw new PanimError(`${name}: row ${y + 1} has ${r.length} columns, expected ${w}`);
    if (!/^[#+.]*$/.test(r)) throw new PanimError(`${name}: row ${y + 1} has a character other than # + .`);
    for (let x = 0; x < w; x++) px[y * w + x] = r[x] === "#" ? 1 : r[x] === "+" ? 2 : 0;
  });
  return { w, h: rows.length, px };
}

/** The value of a per-frame number at timeline frame t. */
export function numAt(n: Num, t: number, frames: number, what: string): number {
  if (typeof n === "number") return n;
  if (Array.isArray(n)) {
    if (n.length !== frames) throw new PanimError(`${what}: ${n.length} values, expected ${frames}`);
    return n[t]!;
  }
  const lo = n.lo ?? 0;
  if (!(n.wrap > 0)) throw new PanimError(`${what}: wrap must be positive`);
  return lo + ((((n.base + n.step * t) % n.wrap) + n.wrap) % n.wrap);
}

/* Draws one sprite into the frame map (values 0/1/2) at (x, y), skipping transparent pixels and anything outside the clip. */
function stamp(map: Uint8Array, w: number, h: number, s: Sprite, x: number, y: number, clip: [number, number, number, number]) {
  const [cx, cy, cw, ch] = clip;
  for (let sy = 0; sy < s.h; sy++) {
    const dy = y + sy;
    if (dy < cy || dy >= cy + ch || dy < 0 || dy >= h) continue;
    for (let sx = 0; sx < s.w; sx++) {
      const v = s.px[sy * s.w + sx]!;
      const dx = x + sx;
      if (v === 0 || dx < cx || dx >= cx + cw || dx < 0 || dx >= w) continue;
      map[dy * w + dx] = v;
    }
  }
}

const shapeValue = (c: string | undefined): number => (c === "+" ? 2 : c === "." ? 0 : 1);

/* Paints the base shapes into map. Every shape is clipped to the canvas. */
export function drawShapes(map: Uint8Array, w: number, h: number, shapes: Shape[]): void {
  const put = (x: number, y: number, v: number) => {
    if (x >= 0 && y >= 0 && x < w && y < h) map[y * w + x] = v;
  };
  for (const sh of shapes) {
    const v = shapeValue((sh as { c?: string }).c);
    if ("fill" in sh) {
      const [x, y, rw, rh] = sh.fill;
      for (let yy = y; yy < y + rh; yy++) for (let xx = x; xx < x + rw; xx++) put(xx, yy, v);
    } else if ("box" in sh) {
      const [x, y, rw, rh] = sh.box;
      for (let xx = x; xx < x + rw; xx++) {
        put(xx, y, v);
        put(xx, y + rh - 1, v);
      }
      for (let yy = y; yy < y + rh; yy++) {
        put(x, yy, v);
        put(x + rw - 1, yy, v);
      }
    } else if ("line" in sh) {
      let [x0, y0] = sh.line;
      const [, , x1, y1] = sh.line;
      const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
      let err = dx + dy;
      for (;;) {
        put(x0, y0, v);
        if (x0 === x1 && y0 === y1) break;
        const e2 = 2 * err;
        if (e2 >= dy) {
          err += dy;
          x0 += sx;
        }
        if (e2 <= dx) {
          err += dx;
          y0 += sy;
        }
      }
    } else if ("disc" in sh || "ring" in sh) {
      const [cx, cy, r] = "disc" in sh ? sh.disc : sh.ring;
      const ring = "ring" in sh;
      for (let yy = cy - r; yy <= cy + r; yy++)
        for (let xx = cx - r; xx <= cx + r; xx++) {
          const d = (xx - cx) * (xx - cx) + (yy - cy) * (yy - cy);
          if (d <= r * r + r && (!ring || d > (r - 1) * (r - 1) + (r - 1))) put(xx, yy, v);
        }
    } else if ("dots" in sh) {
      const [x, y, rw, rh] = sh.dots;
      const every = sh.every ?? 2;
      for (let yy = y; yy < y + rh; yy++)
        for (let xx = x; xx < x + rw; xx++) if ((xx + yy) % every === 0) put(xx, yy, v);
    }
  }
}

/** Renders every timeline frame of a spec into palette-index maps (0 empty, 1 ink, 2 accent). */
export function renderFrames(spec: Spec, sprites: Map<string, Sprite>, base: Sprite | null): Uint8Array[] {
  const [w, h] = spec.size;
  const out: Uint8Array[] = [];
  const under = new Uint8Array(w * h);
  drawShapes(under, w, h, spec.shapes ?? []);
  if (base) stamp(under, w, h, base, 0, 0, [0, 0, w, h]);
  for (let t = 0; t < spec.frames; t++) {
    const map = under.slice();
    for (const tr of spec.tracks) {
      const show = tr.show === undefined ? 1 : typeof tr.show === "number" ? tr.show : tr.show[t];
      if (!show) continue;
      const name = Array.isArray(tr.sprite) ? tr.sprite[t] : tr.sprite;
      const s = sprites.get(name!);
      if (!s) throw new PanimError(`sprite "${name}" not found`);
      const x = numAt(tr.x, t, spec.frames, "x");
      const y = numAt(tr.y, t, spec.frames, "y");
      stamp(map, w, h, s, x, y, tr.clip ?? [0, 0, w, h]);
    }
    out.push(map);
  }
  return out;
}

/** Packs one map into a frame's bytes: mono1 (ink = any non-zero) or indexed4 (value = palette index, 0 transparent). */
function pack(map: Uint8Array, w: number, h: number, indexed: boolean): Uint8Array {
  if (!indexed) {
    const bits = new Uint8Array(frameBytes(w, h));
    for (let y = 0; y < h; y++)
      for (let x = 0; x < w; x++) if (map[y * w + x]) bits[(y * w + x) >> 3]! |= 0x80 >> ((y * w + x) & 7);
    return bits;
  }
  const bpr = Math.ceil(w / 2);
  const out = new Uint8Array(bpr * h);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const v = map[y * w + x]! & 0x0f;
      const o = y * bpr + (x >> 1);
      out[o] = x % 2 === 0 ? (out[o]! & 0x0f) | (v << 4) : (out[o]! & 0xf0) | v;
    }
  return out;
}

export function hexToRgb565(hex: string): number {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/** Dedupes identical frames, expands holds into framesOrder, and sets passive and active counts from the idle and active ranges. */
export function compile(spec: Spec, maps: Uint8Array[]): Panim {
  const [w, h] = spec.size;
  const hasAccent = maps.some((m) => m.includes(2));
  const indexed = hasAccent;
  const unique: Uint8Array[] = [];
  const byKey = new Map<string, number>();
  const order: number[] = [];
  let passive = 0;
  let active = 0;
  const hold = spec.hold ?? [];
  maps.forEach((map, t) => {
    const bytes = pack(map, w, h, indexed);
    const key = Buffer.from(bytes).toString("base64");
    let idx = byKey.get(key);
    if (idx === undefined) {
      idx = unique.length;
      unique.push(bytes);
      byKey.set(key, idx);
    }
    const n = hold[t] ?? 1;
    for (let k = 0; k < n; k++) order.push(idx);
    if (t >= spec.idle[0] && t < spec.idle[1]) passive += n;
    else if (spec.active && t >= spec.active[0] && t < spec.active[1]) active += n;
  });
  if (order.length > PANIM_MAX_POSITIONS) throw new PanimError(`playback has ${order.length} positions, limit ${PANIM_MAX_POSITIONS}`);
  const anim: Panim = {
    width: w,
    height: h,
    frameRate: spec.fps,
    passiveCount: passive,
    activeCount: active,
    framesOrder: order,
    frames: unique,
  };
  if (indexed) {
    anim.pixelFormat = "indexed4";
    anim.palette = [
      { rgb565: 0, monoOverride: 1 }, // value 1: ink
      { rgb565: hexToRgb565(spec.accent ?? "#ff6a2a"), monoOverride: 0 }, // value 2: accent
    ];
  }
  return anim;
}

function checkRange(name: string, r: [number, number] | undefined, frames: number, what: string) {
  if (!r) return;
  if (!(Number.isInteger(r[0]) && Number.isInteger(r[1]) && r[0] >= 0 && r[0] < r[1] && r[1] <= frames))
    throw new PanimError(`${name}: ${what} range ${JSON.stringify(r)} must be 0 <= from < to <= ${frames}`);
}

/** The rules a spec must meet before it compiles: frame count, the idle loop from 0, and active running from the end of idle to the end. */
export function checkSpec(spec: Spec, name: string): void {
  if (!Number.isInteger(spec.frames) || spec.frames < 1 || spec.frames > 255) throw new PanimError(`${name}: frames must be 1..255`);
  checkRange(name, spec.idle, spec.frames, "idle");
  if (spec.idle[0] !== 0) throw new PanimError(`${name}: the idle loop starts at frame 0`);
  if (spec.active) {
    checkRange(name, spec.active, spec.frames, "active");
    if (spec.active[0] !== spec.idle[1] || spec.active[1] !== spec.frames)
      throw new PanimError(`${name}: active must run from the end of idle to the last frame`);
  } else if (spec.idle[1] !== spec.frames) {
    throw new PanimError(`${name}: frames after idle are unused: give them an active range`);
  }
}

/** Reads one animation source directory and compiles it. */
export function compileDir(dir: string, name: string): Panim {
  const spec = JSON.parse(readFileSync(join(dir, "anim.json"), "utf8")) as Spec;
  checkSpec(spec, name);
  const sprites = new Map<string, Sprite>();
  for (const f of readdirSync(dir)) {
    if (!f.endsWith(".txt") || f === "base.txt") continue;
    sprites.set(f.slice(0, -4), parseSprite(readFileSync(join(dir, f), "utf8"), `${name}/${f}`));
  }
  const basePath = join(dir, "base.txt");
  const base = existsSync(basePath) ? parseSprite(readFileSync(basePath, "utf8"), `${name}/base.txt`) : null;
  const [w, h] = spec.size;
  if (base && (base.w !== w || base.h !== h)) throw new PanimError(`${name}: base.txt must be ${w}x${h}`);
  return compile(spec, renderFrames(spec, sprites, base));
}

/* ---------------------------------------------------------------- generated C */

const cname = (name: string) => name.replace(/-/g, "_").toUpperCase();

function bytesC(b: Uint8Array): string {
  const rows: string[] = [];
  for (let i = 0; i < b.length; i += 16)
    rows.push("    " + Array.from(b.subarray(i, i + 16), (x) => "0x" + x.toString(16).padStart(2, "0")).join(", ") + ",");
  return rows.join("\n");
}

/** The generated C: system/src/anims_data.c (the table), one .inc of bytes per animation (they are not .c, so the line limit of
 * ./pal check does not apply to them), and the header. */
export function generateC(anims: { name: string; bytes: Uint8Array }[]): { path: string; text: string }[] {
  const incs = anims.map((a) => ({
    path: join(ROOT, "system/src/anims", `${a.name}.inc`),
    text: `/* GENERATED by forge/tools/panim.ts: the .panim bytes of ${a.name} (see anims_data.c). Do not edit. */\n${bytesC(a.bytes)}\n`,
  }));
  const table = anims.map((a) => `static const uint8_t ANIM_${cname(a.name)}[] = {\n#include "anims/${a.name}.inc"\n};`);
  const entries = anims.map((a) => `    {"${a.name}", ANIM_${cname(a.name)}, sizeof(ANIM_${cname(a.name)})},`);
  const c = [
    "/* GENERATED by forge/tools/panim.ts from assets/anims (one directory per animation, sources in ASCII). Do not edit by hand:",
    " * change the sources and run bun forge/tools/panim.ts (./pal check fails when this file is out of date). The bytes of each",
    " * animation are in system/src/anims/<name>.inc, included here. */",
    '#include "pal/anims_data.h"',
    "",
    ...table,
    "",
    "const pal_anim_blob_t PAL_ANIM_BLOBS[] = {",
    ...entries,
    "};",
    "const size_t PAL_ANIM_BLOBS_COUNT = sizeof(PAL_ANIM_BLOBS) / sizeof(PAL_ANIM_BLOBS[0]);",
    "",
  ].join("\n");
  const h = [
    "/* GENERATED by forge/tools/panim.ts from assets/anims. Do not edit by hand. */",
    "#ifndef PAL_ANIMS_DATA_H",
    "#define PAL_ANIMS_DATA_H",
    "",
    "#include <stddef.h>",
    "#include <stdint.h>",
    "",
    "/* One built-in animation: its name and its .panim bytes (embedded in flash, read in place). */",
    "typedef struct {",
    "    const char *name;",
    "    const uint8_t *data;",
    "    size_t len;",
    "} pal_anim_blob_t;",
    "",
    "extern const pal_anim_blob_t PAL_ANIM_BLOBS[];",
    "extern const size_t PAL_ANIM_BLOBS_COUNT;",
    "",
    "#endif",
    "",
  ].join("\n");
  return [{ path: OUT_C, text: c }, { path: OUT_H, text: h }, ...incs];
}

/* ---------------------------------------------------------------- main */

/** Builds every animation under dir. Errors are collected per animation, not thrown. */
export function buildAll(dir: string): { anims: { name: string; bytes: Uint8Array }[]; errors: string[] } {
  const anims: { name: string; bytes: Uint8Array }[] = [];
  const errors: string[] = [];
  const names = existsSync(dir) ? readdirSync(dir).sort() : [];
  for (const n of names) {
    const p = join(dir, n);
    if (n === "out" || !statSync(p).isDirectory()) continue;
    if (!/^[a-z0-9-]+$/.test(n)) {
      errors.push(`${n}: name must be a-z, 0-9, '-'`);
      continue;
    }
    try {
      anims.push({ name: n, bytes: encodePanim(compileDir(p, n)) });
    } catch (e) {
      errors.push(`${n}: ${(e as Error).message}`);
    }
  }
  return { anims, errors };
}

function main(argv: string[]): number {
  const check = argv.includes("--check");
  const { anims, errors } = buildAll(SRC_DIR);
  if (errors.length) {
    for (const e of errors) console.error(`panim: ${e}`);
    return 1;
  }
  const files: { path: string; text: string | Uint8Array }[] = generateC(anims);
  for (const a of anims) files.push({ path: join(OUT_DIR, `${a.name}.panim`), text: a.bytes });
  if (check) {
    const stale: string[] = [];
    for (const f of files) {
      const want = typeof f.text === "string" ? f.text : Buffer.from(f.text).toString("latin1");
      const have = existsSync(f.path) ? (typeof f.text === "string" ? readFileSync(f.path, "utf8") : Buffer.from(readFileSync(f.path)).toString("latin1")) : null;
      if (have !== want) stale.push(f.path);
    }
    for (const p of stale) console.error(`panim: ${p.slice(ROOT.length + 1)} is out of date; run: bun forge/tools/panim.ts`);
    if (stale.length) return 1;
    console.log(`panim: ${anims.length} animations up to date`);
    return 0;
  }
  mkdirSync(OUT_DIR, { recursive: true });
  mkdirSync(join(ROOT, "system/src/anims"), { recursive: true });
  for (const f of files) writeFileSync(f.path, f.text);
  console.log(`panim: wrote ${anims.length} animations`);
  return 0;
}

if (import.meta.main) process.exit(main(process.argv.slice(2)));
