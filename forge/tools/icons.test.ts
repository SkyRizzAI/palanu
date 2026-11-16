// Tests for forge/tools/icons.ts: the parser, the bit packing, and the checked-in generated files.
import { describe, expect, test } from "bun:test";
import { existsSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { generate, packBits, parseIcon } from "./icons";

const ROOT = resolve(import.meta.dir, "../..");

describe("icon sources", () => {
  test("parses ink and accent planes", () => {
    const ic = parseIcon("dot", 8, "........\n.+.##...\n" + "........\n".repeat(6));
    expect(ic.w).toBe(8);
    expect(ic.ink[1]).toBe(".#.##...");
    expect(ic.accent[1]).toBe(".+......");
  });

  test("rejects a wrong size, a bad row and a stray character", () => {
    expect(() => parseIcon("x", 8, "........\n")).toThrow(/height/);
    expect(() => parseIcon("x", 8, "........\n".repeat(7) + "...\n")).toThrow(/row 8/);
    expect(() => parseIcon("x", 8, "........\n".repeat(7) + "..x.....\n")).toThrow(/character/);
    expect(() => parseIcon("x", 8, "........\n".repeat(7) + "........\n".slice(0, 4) + "\n")).toThrow();
  });

  test("packs rows MSB first and pads each row to whole bytes", () => {
    expect(packBits(["#.......", "........"], "#", 8)).toEqual([0x80, 0x00]);
    expect(packBits(["..#.....#"], "#", 9)).toEqual([0x20, 0x80]);
    expect(packBits(["+#+"], "+", 3)).toEqual([0xa0]);
  });
});

describe("generated output", () => {
  test("a directory with a bad file reports it and generates nothing for it", () => {
    const dir = join(ROOT, "forge/tools/.icons-fixture-missing");
    const g = generate(dir);
    expect(g.count).toBe(0);
    expect(g.errors.length).toBe(0);
  });

  test("the checked-in files match the sources", () => {
    const g = generate(join(ROOT, "assets/icons"));
    expect(g.errors).toEqual([]);
    expect(existsSync(join(ROOT, "system/src/icons_data.c"))).toBe(true);
    expect(readFileSync(join(ROOT, "system/src/icons_data.c"), "utf8")).toBe(g.c);
    expect(readFileSync(join(ROOT, "system/include/pal/icons_data.h"), "utf8")).toBe(g.h);
  });

  test("every launcher icon exists at 32 px and every status icon at 8 px", () => {
    const g = generate(join(ROOT, "assets/icons"));
    for (const name of ["terminal", "settings", "files", "monitor", "ssh", "link", "wifi", "bluetooth", "notifications",
      "clock", "battery", "brightness", "lock", "unlock", "power", "moon", "services", "apps", "logs", "info", "about",
      "store", "default"]) {
      expect(g.c).toContain(`{"${name}", 32, 32, 32,`);
    }
    for (const name of ["wifi0", "wifi4", "bluetooth", "link", "sd", "lock", "bell", "battery0", "battery4"]) {
      expect(g.c).toContain(`{"${name}", 8, 8, 8,`);
    }
  });
});
