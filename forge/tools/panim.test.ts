// Tests for forge/tools/panim.ts: the v1 and v2 layouts against bytes made by a reference encoder, the reader's rejections,
// the compiler (holds, idle and active ranges, dedupe), the shape and track rules, and the checked-in outputs.
import { describe, expect, test } from "bun:test";
import { existsSync } from "node:fs";
import { join, resolve } from "node:path";
import {
  buildAll,
  checkSpec,
  compile,
  decodePanim,
  drawShapes,
  encodePanim,
  encodeV1,
  encodeV2,
  numAt,
  PanimError,
  parseSprite,
  type Panim,
  renderFrames,
  type Spec,
} from "./panim";

const ROOT = resolve(import.meta.dir, "../..");

// Encoded by an independent reference encoder for the content in the tests below. Files from it must load unchanged.
const REF_V1 = Uint8Array.from([
  0x50, 0x41, 0x4e, 0x4d, 0x01, 0x0c, 0x00, 0x04, 0x00, 0x08, 0x03, 0x02, 0x03, 0x05, 0x00, 0x01, 0x01, 0x02, 0x00, 0xf0,
  0x0f, 0xa0, 0x50, 0x0f, 0xf0, 0xff, 0xf0, 0x0f, 0xf0, 0x55, 0xaa, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x80,
  0x01, 0x12, 0x34,
]);
const REF_V2 = Uint8Array.from([
  0x50, 0x41, 0x4e, 0x4d, 0x02, 0x05, 0x00, 0x03, 0x00, 0x04, 0x02, 0x00, 0x02, 0x02, 0x01, 0x00, 0x02, 0x00, 0xf8, 0x00,
  0xff, 0xff, 0x01, 0x00, 0x01, 0x12, 0x30, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x00, 0x21, 0x00, 0x00,
  0x00, 0x00, 0x00,
]);

const bytes = (...v: number[]) => Uint8Array.from(v);

describe("layouts match the reference codec", () => {
  test("v1 mono: decoding the reference bytes gives the content, and encoding gives the same bytes back", () => {
    const a = decodePanim(REF_V1);
    expect([a.width, a.height, a.frameRate, a.passiveCount, a.activeCount]).toEqual([12, 4, 8, 3, 2]);
    expect(a.framesOrder).toEqual([0, 1, 1, 2, 0]);
    expect(a.frames.length).toBe(3);
    expect(a.frames[0]).toEqual(bytes(0xf0, 0x0f, 0xa0, 0x50, 0x0f, 0xf0, 0xff, 0xf0));
    expect(encodeV1(a)).toEqual(REF_V1);
  });

  test("v2 indexed4: palette, nibbles (even x high) and the same bytes back", () => {
    const a = decodePanim(REF_V2);
    expect(a.pixelFormat).toBe("indexed4");
    expect(a.palette).toEqual([
      { rgb565: 0xf800, monoOverride: 0 },
      { rgb565: 0xffff, monoOverride: 1 },
    ]);
    expect(a.frames[0]![0]).toBe(0x12); // pixels 1 and 2 of row 0
    expect(encodeV2(a)).toEqual(REF_V2);
    expect(encodePanim(a)).toEqual(REF_V2);
  });

  test("a mono animation is written as v1 (plays on any firmware), an indexed one as v2", () => {
    const mono: Panim = { width: 8, height: 2, frameRate: 4, passiveCount: 1, activeCount: 0, framesOrder: [], frames: [bytes(0x81, 0x42)] };
    expect(encodePanim(mono)[4]).toBe(1);
    const idx: Panim = {
      width: 2, height: 1, frameRate: 4, passiveCount: 1, activeCount: 0, framesOrder: [], frames: [bytes(0x12)],
      pixelFormat: "indexed4", palette: [{ rgb565: 0, monoOverride: 1 }, { rgb565: 0xf800, monoOverride: 0 }],
    };
    expect(encodePanim(idx)[4]).toBe(2);
  });
});

describe("the reader rejects bad input", () => {
  test("wrong magic, unknown version, truncated header, truncated frames, bad order, bad palette", () => {
    const magic = REF_V1.slice();
    magic[0] = 0x51;
    expect(() => decodePanim(magic)).toThrow(PanimError);
    const ver = REF_V1.slice();
    ver[4] = 3;
    expect(() => decodePanim(ver)).toThrow(/version 3/);
    expect(() => decodePanim(REF_V1.slice(0, 10))).toThrow();
    expect(() => decodePanim(REF_V1.slice(0, REF_V1.length - 1))).toThrow(/truncated frame/);
    const order = REF_V1.slice();
    order[14] = 9; // first framesOrder entry out of range
    expect(() => decodePanim(order)).toThrow(/out of range/);
    const pal = REF_V2.slice();
    pal[16] = 0; // palette count 0
    expect(() => decodePanim(pal)).toThrow(/palette/);
    expect(() => decodePanim(bytes(0x50, 0x41))).toThrow(/too short/);
  });
});

describe("the compiler", () => {
  const spec = (over: Partial<Spec>): Spec => ({
    fps: 4,
    size: [8, 2],
    frames: 3,
    idle: [0, 2],
    active: [2, 3],
    hold: [2, 1, 1],
    tracks: [{ sprite: "dot", x: 0, y: [0, 0, 0] }],
    ...over,
  });
  const sprites = new Map([["dot", parseSprite("#\n", "dot")]]);

  test("holds become repeated positions; passive and active counts follow the ranges", () => {
    // Frame 0 holds twice, frame 1 once (idle), frame 2 once (active). Frames 0 and 1 are identical, so they share a bitmap.
    const maps = renderFrames(spec({}), sprites, null);
    const a = compile(spec({}), maps);
    expect(a.frames.length).toBe(1);
    expect(a.framesOrder).toEqual([0, 0, 0, 0]);
    expect(a.passiveCount).toBe(3);
    expect(a.activeCount).toBe(1);
  });

  test("distinct frames keep their order; accent pixels make the animation indexed4", () => {
    const sp = new Map([["dot", parseSprite("+#\n", "dot")]]);
    const s = spec({ hold: undefined, tracks: [{ sprite: "dot", x: [0, 2, 4], y: 0 }] });
    const a = compile(s, renderFrames(s, sp, null));
    expect(a.pixelFormat).toBe("indexed4");
    expect(a.frames.length).toBe(3);
    expect(a.framesOrder).toEqual([0, 1, 2]);
    expect(a.passiveCount).toBe(2);
    expect(a.activeCount).toBe(1);
    expect(a.palette!.length).toBe(2);
  });

  test("the rules: the idle loop starts at 0, active follows idle and reaches the last frame", () => {
    const sp = new Map([["dot", parseSprite("#\n", "dot")]]);
    const bad = (over: Partial<Spec>) => () => checkSpec(spec(over), "t");
    expect(() => checkSpec(spec({}), "t")).not.toThrow();
    expect(bad({ idle: [1, 2] })).toThrow(/starts at frame 0/);
    expect(bad({ active: [1, 3] })).toThrow(/end of idle/);
    expect(bad({ active: [2, 2] })).toThrow(/range/);
    expect(bad({ active: undefined, idle: [0, 2] })).toThrow(/unused/);
  });

  test("sprites: transparent dots, clipping, rejects of ragged and stray characters", () => {
    const s = parseSprite("#.+\n..#\n", "t");
    expect([s.w, s.h]).toEqual([3, 2]);
    expect(Array.from(s.px)).toEqual([1, 0, 2, 0, 0, 1]);
    expect(() => parseSprite("##\n#\n", "t")).toThrow(/columns/);
    expect(() => parseSprite("#x\n", "t")).toThrow(/character/);
    const big = parseSprite("####\n####\n", "t");
    // clip to x 0..1 only: sprite is drawn partly
    const s2: Spec = spec({ size: [4, 2], tracks: [{ sprite: "t", x: 0, y: 0, clip: [0, 0, 2, 2] }] });
    const f = renderFrames(s2, new Map([["t", big]]), null);
    expect(Array.from(f[0]!)).toEqual([1, 1, 0, 0, 1, 1, 0, 0]);
  });

  test("numbers: per frame arrays, wrap with a step, and bad inputs", () => {
    expect(numAt(5, 3, 4, "x")).toBe(5);
    expect(numAt([1, 2, 3], 2, 3, "x")).toBe(3);
    expect(() => numAt([1, 2], 0, 3, "x")).toThrow(/values/);
    // lo + ((base + step * t) mod wrap): step -2 over wrap 8 from 0 walks 0, 6, 4, 2, 0 ...
    const w = { base: 0, step: -2, wrap: 8, lo: 0 };
    expect([0, 1, 2, 3, 4].map((t) => numAt(w, t, 9, "y"))).toEqual([0, 6, 4, 2, 0]);
  });

  test("shapes: a box is an outline, a disc is filled, erase clears ink", () => {
    const m = new Uint8Array(8 * 8);
    drawShapes(m, 8, 8, [{ box: [0, 0, 4, 4] }, { fill: [1, 1, 2, 2], c: "." }, { disc: [6, 6, 1] }]);
    expect(m[0]).toBe(1); // corner of the box
    expect(m[1 * 8 + 1]).toBe(0); // erased inside
    expect(m[6 * 8 + 6]).toBe(1); // disc centre
    expect(m[1 * 8 + 6]).toBe(0); // outside everything
  });
});

describe("the checked-in outputs", () => {
  test("every source builds, and the built animations round-trip through the reader", () => {
    const { anims, errors } = buildAll(join(ROOT, "assets/anims"));
    expect(errors).toEqual([]);
    expect(anims.map((a) => a.name).sort()).toEqual(["coder", "coffee", "desk-hacker", "lab", "laptop-cat", "rain", "rocket", "server", "sleepy-cat"]);
    for (const a of anims) {
      const d = decodePanim(a.bytes);
      expect(d.frames.length).toBeGreaterThanOrEqual(1);
      expect(d.frames.length).toBeLessThanOrEqual(24);
      expect(d.width).toBeGreaterThan(0);
      expect(d.height).toBeGreaterThan(0);
      expect(d.framesOrder.length).toBeLessThanOrEqual(255);
      expect(encodePanim(d)).toEqual(a.bytes);
    }
  });
});
