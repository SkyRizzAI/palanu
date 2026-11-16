// Parser and report checks for forge/tools/hwtest.ts. No device or simulator needed.
//   bun test forge/tests/hwtest.test.ts
import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fmtValue, parseArgs, parseBle, parseBoot, parseFree, parseMem, parsePerf, parsePerfKeys, parsePerfTargets, parseTop, parseWifi, renderSection, tableKb } from "../tools/hwtest.ts";

const near = (a: number | null, b: number) => expect(a).not.toBeNull() && expect(Math.abs((a as number) - b)).toBeLessThan(0.01);

test("boot: home and total in ms, seconds converted", () => {
  const out = "stage nvs 12 ms\nstage fs 240 ms\nhome shown at 1834 ms\ntotal 2.01 s";
  const b = parseBoot(out);
  near(b.bootHomeMs, 1834);
  near(b.bootTotalMs, 2010);
  near(parseBoot("kernel 40 ms\ndesktop 1200 ms").bootHomeMs, 1200); // desktop is the home screen stage
});

test("mem: bytes, KB, MB converted to KB; per-owner and largest-block lines do not feed internal free", () => {
  const out = [
    "internal free: 98304 bytes",
    "min free: 90112 bytes",
    "largest block: 40 KB",
    "dma free: 20 KB",
    "psram free: 3.5 MB",
  ].join("\n");
  const m = parseMem(out);
  near(m.internalFreeKb, 96);
  near(m.minFreeKb, 88);
  near(m.largestKb, 40);
  near(m.dmaFreeKb, 20);
  near(m.psramFreeKb, 3584);
});

test("mem: internal free is not taken from the largest-block line when it comes first", () => {
  const m = parseMem("largest internal block 32 KB\ninternal free 90 KB");
  near(m.internalFreeKb, 90);
  near(m.largestKb, 32);
});

test("mem and free as column tables (the device's real layout)", () => {
  const mem = [
    "memory     total   free    min  block",
    "internal    459K   419K   419K   314K",
    "dma         459K   419K   419K   314K",
    "external  16384K 16270K 16270K 12203K",
    "floor 32K kept free; dma is part of internal",
    "",
    "owner        int+dma  external    stack  allocs",
    "kernel             0     16464        0       6",
  ].join("\n");
  const m = parseMem(mem);
  near(m.internalFreeKb, 419);
  near(m.minFreeKb, 419);
  near(m.largestKb, 314);
  near(m.dmaFreeKb, 419);
  near(m.psramFreeKb, 16270);
  const free = [
    "memory      total    free     largest min free",
    "internal     459K    419K     419K     419K",
    "external   16384K  16270K   16270K   16270K",
  ].join("\n");
  near(parseFree(free).internalFreeKb, 419);
  near(parseFree(free).psramFreeKb, 16270);
  expect(tableKb(free, "internal", "nope")).toBeNull();
});

test("free: falls back to internal and psram lines", () => {
  const f = parseFree("heap internal free 120 KB\npsram free 2048 KB");
  near(f.internalFreeKb, 120);
  near(f.psramFreeKb, 2048);
});

test("top: cpu percent from idle", () => {
  near(parseTop("CPU idle 97.5 %").cpuPct, 2.5);
  near(parseTop("cpu 4 %").cpuPct, 4);
  near(parseTop(" top  up 00:00:01  cpu0   0% cpu1  10% ").cpuPct, 5); // mean over cores
  expect(parseTop("nothing here").cpuPct).toBeNull();
});

test("perf: draw, flush, fps in both orders", () => {
  const p = parsePerf("draw 4.2 ms  flush 11.8 ms  fps 58.3");
  near(p.drawMs, 4.2);
  near(p.flushMs, 11.8);
  near(p.fps, 58.3);
  near(parsePerf("57.9 fps").fps, 57.9);
});

test("perf keys: p50, p95, max", () => {
  const k = parsePerfKeys("keys 100 p50 18 ms p95 27 ms max 41 ms");
  near(k.keyP50Ms, 18);
  near(k.keyP95Ms, 27);
  near(k.keyMaxMs, 41);
});

test("radios: connected, not connected, and unknown output", () => {
  expect(parseWifi("wifi: connected to HomeNet, 192.168.1.20")).toBe(true);
  expect(parseWifi("wifi: not connected")).toBe(false);
  expect(parseWifi("wifi: disconnected")).toBe(false);
  expect(parseWifi("unknown command: wifi")).toBeNull(); // unknown output is not "connected"
  expect(parseBle("ble: connected (1 client)")).toBe(true);
  expect(parseBle("ble: not connected")).toBe(false);
  expect(parseBle("")).toBeNull();
});

test("unparseable output gives nulls, never NaN or a guess", () => {
  const garbage = "boot: n/a\nmem: ????\nlorem ipsum";
  expect(parseBoot(garbage)).toEqual({ bootHomeMs: null, bootTotalMs: null });
  expect(parseMem("")).toEqual({ internalFreeKb: null, minFreeKb: null, largestKb: null, dmaFreeKb: null, psramFreeKb: null });
  expect(parseTop(garbage).cpuPct).toBeNull();
  expect(parsePerf("draw: n/a  flush: n/a")).toEqual({ drawMs: null, flushMs: null, fps: null });
  expect(parsePerfKeys("no data")).toEqual({ keyP50Ms: null, keyP95Ms: null, keyMaxMs: null });
  expect(fmtValue(null, "ms")).toBe("n/a");
});

test("PERF targets are read from perf-targets.json by exact name", () => {
  const src = readFileSync(join(import.meta.dir, "../tools/perf-targets.json"), "utf8");
  const t = parsePerfTargets(src);
  expect(t.get("Boot until home is shown")).toContain("2.5 s");
  expect(t.get("CPU at idle on home")).toBe("≤ 2 %");
  expect(t.get("Free internal RAM, everything on")).toContain("86 KB");
  expect(t.get("missing")).toBeUndefined();
});

test("section: n/a for missing values, raw output in details, skipped status", () => {
  const perf = new Map([["CPU at idle on home", "≤ 2 %"]]);
  const md = renderSection({
    when: "2026-01-01 00:00 UTC", scenario: "idle", board: "sim (host), ver 0, build x", target: "tcp 127.0.0.1:7100",
    skipped: [], values: { cpuPct: 1.25 }, perfTargets: perf, raw: [["top", "```evil```"]],
  });
  expect(md).toContain("| CPU at idle on home | 1.3 % | ≤ 2 % |");
  expect(md).toContain("| Key p50 | n/a | - |");
  expect(md).toContain("<details>");
  expect(md).not.toContain("```evil```"); // raw fences cannot break the markdown
  const skipped = renderSection({
    when: "t", scenario: "wifi", board: "b", target: "t", skipped: ["connect Wi-Fi first"], values: {}, perfTargets: perf, raw: [],
  });
  expect(skipped).toContain("**skipped**");
  expect(skipped).toContain("connect Wi-Fi first");
});

test("args: defaults to the simulator, scenario validation, all", () => {
  const d = parseArgs([]);
  expect(d.target).toEqual({ kind: "tcp", host: "127.0.0.1", port: 7100 });
  expect(d.scenarios).toEqual(["idle"]);
  expect(parseArgs(["--serial", "/dev/cu.x@230400", "--commit"]).target).toEqual({ kind: "serial", path: "/dev/cu.x", baud: 230400 });
  expect(parseArgs(["--scenario", "all"]).scenarios).toEqual(["idle", "wifi", "ble", "both", "full"]);
  expect(() => parseArgs(["--scenario", "nope"])).toThrow();
  expect(() => parseArgs(["--tcp", "nohost"])).toThrow();
});
