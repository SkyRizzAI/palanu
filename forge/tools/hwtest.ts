#!/usr/bin/env bun
// PalanuOS baseline measurements. Runs the device's own commands over the Palanu Link shell channel, parses the
// numbers, and appends a dated section to docs/SIZE-BUDGETS.md (the one place for measured numbers).
//   bun forge/tools/hwtest.ts [--tcp host:port | --serial /dev/cu.x[@baud]] [--scenario idle|wifi|ble|both|full|all]
//                             [--out docs/SIZE-BUDGETS.md] [--commit]
// Default target: the simulator at 127.0.0.1:7100 (start it with: ./pal sim --link 7100 --link-open --wait 600000).
// Device output formats are not final. Every parser takes raw text, matches lines by label, and returns null when it
// finds nothing; null prints as "n/a". The raw output always goes into the section's <details>.
import { appendFileSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { Link, authenticate, connectSerial, connectTcp } from "../packages/link/src/index.ts";

const ROOT = resolve(import.meta.dir, "../..");
const PERF_SRC = join(ROOT, "forge/tools/perf-targets.json");
const PAL_KEY_RIGHT = 10; // system/include/pal/input.h: NONE=0, CHAR=1, ENTER=2, ESC=3, BACKSPACE=4, TAB=5, DELETE=6, UP=7, DOWN=8, LEFT=9, RIGHT=10
const KEY_COUNT = 100;
const KEY_SPACING_MS = 50;
const te = new TextEncoder(), td = new TextDecoder();
const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

// ------------------------------------------------------------------ parsers (pure, exported for tests)
type Found = { v: number; unit: string };
const NUM = "(-?\\d+(?:[.,]\\d+)?)";
const toNum = (s: string) => Number(s.replace(",", "."));

/** First number after `label` on the first line that contains it (lines matching `skip` are ignored). */
export function find(out: string, label: RegExp, skip?: RegExp): Found | null {
  for (const line of out.split("\n")) {
    if (skip?.test(line) || !label.test(line)) continue;
    const m = line.match(new RegExp("(?:" + label.source + ")[^\\d\\n]*?" + NUM + "\\s*([A-Za-zµ%/]*)", "i"));
    if (m) return { v: toNum(m[1]), unit: m[2].toLowerCase() };
  }
  return null;
}

/** Converts to milliseconds. A missing unit means milliseconds. */
const toMs = (f: Found | null): number | null => {
  if (!f) return null;
  if (f.unit === "us" || f.unit === "µs") return f.v / 1000;
  if (f.unit === "s" || f.unit === "sec") return f.v * 1000;
  return f.v;
};

/** Converts to KB. A missing unit means bytes (heap APIs report bytes). */
const toKb = (f: Found | null): number | null => {
  if (!f) return null;
  if (f.unit === "mb" || f.unit === "mib") return f.v * 1024;
  if (f.unit === "" || f.unit === "b" || f.unit === "byte" || f.unit === "bytes") return f.v / 1024;
  return f.v; // kb, kib, k
};

const pct = (f: Found | null): number | null => f?.v ?? null;

/** "459K", "16384K", "3.5M", "1024" (no suffix = bytes) -> KB. */
const sizeKb = (tok: string | undefined): number | null => {
  const m = tok?.match(/^(-?\d+(?:\.\d+)?)([KMG]?)B?$/i);
  if (!m) return null;
  const v = Number(m[1]), u = m[2].toUpperCase();
  return u === "G" ? v * 1024 * 1024 : u === "M" ? v * 1024 : u === "K" ? v : v / 1024;
};

/**
 * Cell of a column table: the header line names the columns (its first word labels the row column), and the row
 * whose first word is `row` gives the values. Example: "memory total free min block" / "internal 459K 419K 419K 314K".
 */
export function tableKb(out: string, row: string, col: string): number | null {
  const rows = out.split("\n").map((l) => l.trim().split(/\s+/));
  const head = rows.find((t) => t.includes(col) && !t.slice(1).some((x) => /\d/.test(x)));
  if (!head) return null;
  const line = rows.find((t) => t[0] === row && t.length > 1);
  return line ? sizeKb(line[head.indexOf(col)]) : null;
}

export function parseBoot(out: string) {
  // The home screen is the "desktop" stage in pal_boot_report; "home" is accepted too.
  return { bootHomeMs: toMs(find(out, /home|desktop/)), bootTotalMs: toMs(find(out, /total/)) };
}

export function parseMem(out: string) {
  const sub = /largest|block|min|owner/i;
  return {
    internalFreeKb: tableKb(out, "internal", "free") ?? toKb(find(out, /internal/, sub)),
    minFreeKb: tableKb(out, "internal", "min") ?? toKb(find(out, /min/)),
    largestKb: tableKb(out, "internal", "block") ?? toKb(find(out, /largest|block/)),
    dmaFreeKb: tableKb(out, "dma", "free") ?? toKb(find(out, /dma/)),
    psramFreeKb: tableKb(out, "external", "free") ?? toKb(find(out, /psram|spiram/)),
  };
}

/** `free` is a fallback for internal RAM when `mem` is missing. */
export function parseFree(out: string) {
  return {
    internalFreeKb: tableKb(out, "internal", "free") ?? toKb(find(out, /internal|heap|ram/, /largest|block|psram|spiram/)),
    psramFreeKb: tableKb(out, "external", "free") ?? toKb(find(out, /psram|spiram/)),
  };
}

/** `top` shows "cpu0 0%  cpu1 0%" per frame; idle percent is used when present. CPU is the mean over cores. */
export function parseTop(out: string) {
  const idle = pct(find(out, /idle/));
  if (idle !== null) return { cpuPct: 100 - idle };
  const cores = [...out.matchAll(/\bcpu\d*\s+(\d+(?:[.,]\d+)?)\s*%/gi)].map((m) => toNum(m[1]));
  return { cpuPct: cores.length ? cores.reduce((a, b) => a + b, 0) / cores.length : null };
}

export function parsePerf(out: string) {
  const fpsBefore = out.match(new RegExp(NUM + "\\s*fps", "i")); // "58.3 fps"
  const fps = find(out, /fps/)?.v ?? (fpsBefore ? toNum(fpsBefore[1]) : null);
  return { drawMs: toMs(find(out, /draw/)), flushMs: toMs(find(out, /flush/)), fps };
}

export function parsePerfKeys(out: string) {
  return { keyP50Ms: toMs(find(out, /p50/)), keyP95Ms: toMs(find(out, /p95/)), keyMaxMs: toMs(find(out, /max/)) };
}

/** true, false, or null when the text says neither (unknown command, empty output). */
export function parseUp(out: string, up: RegExp, down: RegExp): boolean | null {
  const t = out.trim();
  if (!t) return null;
  if (down.test(t)) return false;
  if (up.test(t)) return true;
  return null;
}
export const parseWifi = (out: string) =>
  parseUp(out, /\bconnected\b/i, /not connected|disconnected/i);
export const parseBle = (out: string) =>
  parseUp(out, /\bconnected\b/i, /not connected|disconnected/i);

/** Name -> target text from the entries of perf-targets.json ([{ name, target }]). */
export function parsePerfTargets(src: string): Map<string, string> {
  const m = new Map<string, string>();
  for (const e of JSON.parse(src) as { name: string; target: string }[]) m.set(e.name, e.target);
  return m;
}

// ------------------------------------------------------------------ scenarios (data)
type Radio = "wifi" | "ble";
type Step = "screen" | "boot" | "mem" | "free" | "top" | "perf" | "keys";
const BASE: Step[] = ["boot", "mem", "free", "top", "perf", "keys"];
export const SCENARIOS: Record<string, { title: string; radios: Radio[]; steps: Step[] }> = {
  idle: { title: "idle, no radios", radios: [], steps: BASE },
  wifi: { title: "Wi-Fi connected", radios: ["wifi"], steps: BASE },
  ble: { title: "BLE client connected", radios: ["ble"], steps: BASE },
  both: { title: "Wi-Fi and BLE connected", radios: ["wifi", "ble"], steps: BASE },
  full: { title: "everything on: Wi-Fi, BLE, screen link", radios: ["wifi", "ble"], steps: ["screen", ...BASE] },
};
const RADIO_HINT: Record<Radio, { check: string; hint: string }> = {
  wifi: { check: "wifi status", hint: "start Wi-Fi and connect on the device first: wifi on, then wifi connect SSID PASSWORD (this script does not enter the password)" },
  ble: { check: "ble status", hint: "start BLE on the device first (ble on), then connect a BLE client (for example nRF Connect)" },
};

// ------------------------------------------------------------------ report rows
type Unit = "ms" | "KB" | "%" | "fps";
type Row = { metric: string; key?: string; unit: Unit; prefix?: string; perf?: boolean };
// `perf` rows use the exact name in perf-targets.json, so the target comes from there.
export const ROWS: Row[] = [
  { metric: "Key to screen, system and app screens", key: "keyP95Ms", unit: "ms", prefix: "p95 ", perf: true },
  { metric: "Full-screen send", key: "flushMs", unit: "ms", perf: true },
  { metric: "Boot until home is shown", key: "bootHomeMs", unit: "ms", perf: true },
  { metric: "CPU at idle on home", key: "cpuPct", unit: "%", perf: true },
  { metric: "Opening a built-in app", unit: "ms", perf: true },
  { metric: "Opening a 100 KB .papp app", unit: "ms", perf: true },
  { metric: "Terminal output to screen", unit: "KB", perf: true },
  { metric: "Palanu Link over Wi-Fi", unit: "KB", perf: true },
  { metric: "Free internal RAM, everything on", key: "internalFreeKb", unit: "KB", perf: true },
  { metric: "Key p50", key: "keyP50Ms", unit: "ms" },
  { metric: "Key max", key: "keyMaxMs", unit: "ms" },
  { metric: "Internal RAM, largest block", key: "largestKb", unit: "KB" },
  { metric: "Internal RAM, minimum free", key: "minFreeKb", unit: "KB" },
  { metric: "DMA RAM free", key: "dmaFreeKb", unit: "KB" },
  { metric: "PSRAM free", key: "psramFreeKb", unit: "KB" },
  { metric: "Boot total", key: "bootTotalMs", unit: "ms" },
  { metric: "Draw", key: "drawMs", unit: "ms" },
  { metric: "FPS", key: "fps", unit: "fps" },
];

const fmtNum = (v: number) => (Number.isInteger(v) ? String(v) : v.toFixed(1));
export function fmtValue(v: number | null | undefined, unit: Unit, prefix = ""): string {
  if (v === null || v === undefined) return "n/a";
  return `${prefix}${fmtNum(v)}${unit === "%" ? " %" : " " + unit}`;
}

export type Values = Record<string, number | null>;

/** One markdown section for one scenario run. Pure: the caller supplies everything. */
export function renderSection(o: {
  when: string; scenario: string; board: string; target: string; commit?: string;
  skipped: string[]; values: Values; perfTargets: Map<string, string>; raw: [string, string][];
}): string {
  const head = `## ${o.when} · ${o.scenario}${o.commit ? ` (commit ${o.commit})` : ""}`;
  const lines = [head, "", `- Board: ${o.board}`, `- Link: ${o.target}`, `- Scenario: ${SCENARIOS[o.scenario]?.title ?? o.scenario}`];
  if (o.skipped.length) {
    lines.push("- Status: **skipped**");
    for (const s of o.skipped) lines.push(`  - ${s}`);
  } else lines.push("- Status: measured");
  lines.push("", "| metric | value | target |", "|---|---|---|");
  for (const r of ROWS) {
    const target = r.perf ? o.perfTargets.get(r.metric) ?? "-" : "-";
    const value = o.skipped.length || !r.key ? "n/a" : fmtValue(o.values[r.key], r.unit, r.prefix);
    lines.push(`| ${r.metric} | ${value} | ${target} |`);
  }
  lines.push("", "<details><summary>Raw device output</summary>", "");
  for (const [name, text] of o.raw) {
    lines.push(`**${name}**`, "", "```text", (text.trim() || "(empty)").replace(/```/g, "'''"), "```", "");
  }
  lines.push("</details>", "");
  return lines.join("\n");
}

const HEADER = `# PalanuOS size and measurement budgets

This is the one place where measured numbers are kept. Every run of \`./pal hwtest\` (or \`bun forge/tools/hwtest.ts\`)
appends a new dated section below; old sections are never changed. Targets come from
\`forge/tools/perf-targets.json\`.

Simulator numbers do not represent the board. Each section names the board or simulator used.
`;

// ------------------------------------------------------------------ shell over the Link tty channel
const clean = (s: string) => s.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "").replace(/\r/g, "");

class Shell {
  private buf = "";
  private n = 0;
  constructor(private link: Link) {
    link.onTty = (d) => { this.buf += td.decode(d); };
  }
  async open() {
    this.link.openShell(100, 30);
    await sleep(800);
    this.buf = "";
  }
  /** Runs one line; output is the text between two marker lines that the shell prints around the command. */
  /** `until` + `quit`: for live commands (top), send `quit` once `until` matches the output so far. */
  async run(cmd: string, ms = 15000, live?: { until: RegExp; quit: string }): Promise<string> {
    const id = ++this.n, b = `__HWB${id}__`, e = `__HWE${id}__`;
    this.buf = "";
    this.link.ttyWrite(te.encode(`echo ${b}; ${cmd}; echo ${e}\n`));
    const endRe = new RegExp(`^${e}\\s*$`, "m");
    const t0 = Date.now();
    let quit = !live;
    while (!endRe.test(clean(this.buf)) && Date.now() - t0 < ms) {
      if (!quit && live!.until.test(clean(this.buf))) {
        quit = true;
        this.link.ttyWrite(te.encode(live!.quit));
      }
      await sleep(30);
    }
    const text = clean(this.buf);
    const s = text.search(new RegExp(`^${b}\\s*$`, "m")), end = text.search(endRe);
    if (s < 0 || end < 0) {
      this.link.ttyWrite(te.encode("\x03")); // Ctrl-C: do not leave a stuck command for the next one
      await sleep(300);
      return `${text.trim()}\n[no end marker after ${ms} ms]`.trim();
    }
    return text.slice(s + b.length, end).trim();
  }
}

// ------------------------------------------------------------------ CLI
export type Target = { kind: "tcp"; host: string; port: number } | { kind: "serial"; path: string; baud: number };
export type Opts = { target: Target; scenarios: string[]; out: string; commit: boolean };

export function parseArgs(argv: string[]): Opts {
  const o: Opts = { target: { kind: "tcp", host: "127.0.0.1", port: 7100 }, scenarios: ["idle"], out: join(ROOT, "docs/SIZE-BUDGETS.md"), commit: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => {
      const x = argv[++i];
      if (x === undefined) throw new Error(`${a} needs a value`);
      return x;
    };
    if (a === "--tcp") {
      const t = val(), k = t.lastIndexOf(":"), port = Number(t.slice(k + 1));
      if (k < 0 || !Number.isInteger(port)) throw new Error("--tcp takes host:port");
      o.target = { kind: "tcp", host: t.slice(0, k), port };
    } else if (a === "--serial") {
      const [path, baud] = val().split("@");
      o.target = { kind: "serial", path, baud: Number(baud ?? 115200) };
    } else if (a === "--scenario") {
      const s = val();
      if (s === "all") o.scenarios = Object.keys(SCENARIOS);
      else if (s in SCENARIOS) o.scenarios = [s];
      else throw new Error(`unknown scenario ${s} (${Object.keys(SCENARIOS).join("|")}|all)`);
    } else if (a === "--out") o.out = resolve(val());
    else if (a === "--commit") o.commit = true;
    else throw new Error(`unknown option ${a}`);
  }
  return o;
}

async function openLink(t: Target): Promise<Link> {
  if (t.kind === "tcp") return new Link(await connectTcp(t.host, t.port));
  return new Link(connectSerial(t.path, t.baud));
}

async function runStep(step: Step, link: Link, sh: Shell, raw: [string, string][], values: Values) {
  const cmd = async (name: string, ms?: number, live?: { until: RegExp; quit: string }) => {
    const out = await sh.run(name, ms, live);
    raw.push([name, out]);
    return out;
  };
  switch (step) {
    case "screen":
      link.subscribeScreen(true);
      raw.push(["screen", "subscribed to the screen channel"]);
      await sleep(1000);
      return;
    case "boot": Object.assign(values, parseBoot(await cmd("boot"))); return;
    case "mem": Object.assign(values, parseMem(await cmd("mem"))); return;
    case "free": {
      const f = parseFree(await cmd("free"));
      values.internalFreeKb ??= f.internalFreeKb;
      values.psramFreeKb ??= f.psramFreeKb;
      return;
    }
    case "top": // a live monitor: one snapshot (the "tasks" line ends a frame), then q
      Object.assign(values, parseTop(await cmd("top", 15000, { until: /tasks\s+\d+/, quit: "q" })));
      return;
    case "perf": Object.assign(values, parsePerf(await cmd("perf"))); return;
    case "keys":
      await cmd("perf reset"); // the key ring is cumulative; start from zero
      for (let i = 0; i < KEY_COUNT; i++) {
        link.key(PAL_KEY_RIGHT);
        await sleep(KEY_SPACING_MS);
      }
      Object.assign(values, parsePerfKeys(await cmd("perf keys", 20000)));
      return;
  }
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const perfTargets = parsePerfTargets(readFileSync(PERF_SRC, "utf8"));
  const link = await openLink(opts.target);
  const hello = await link.hello();
  // a wireless device needs its paired token (palanu pair); the simulator for this test runs with --link-open
  const info = hello.trusted === true ? hello : await authenticate(link, hello, opts.target.kind === "tcp" ? `palanu pair --tcp ${opts.target.host}:${opts.target.port}` : `palanu pair --serial ${opts.target.path}`);
  const sh = new Shell(link);
  await sh.open();
  const board = `${info.board} (${info.chip}), ver ${info.ver}, build ${info.build || "-"}`;
  const target = opts.target.kind === "tcp" ? `tcp ${opts.target.host}:${opts.target.port}` : `serial ${opts.target.path}`;
  const commit = opts.commit ? Bun.spawnSync(["git", "rev-parse", "--short", "HEAD"], { cwd: ROOT }).stdout.toString().trim() : undefined;
  const when = new Date().toISOString().slice(0, 16).replace("T", " ") + " UTC";
  if (!existsSync(opts.out)) writeFileSync(opts.out, HEADER);

  for (const id of opts.scenarios) {
    const scn = SCENARIOS[id];
    const raw: [string, string][] = [["ctl INFO", JSON.stringify(info, null, 2)]];
    const values: Values = {};
    const skipped: string[] = [];
    for (const r of scn.radios) {
      const out = await sh.run(RADIO_HINT[r].check);
      raw.push([RADIO_HINT[r].check, out]);
      const up = r === "wifi" ? parseWifi(out) : parseBle(out);
      if (!up) skipped.push(RADIO_HINT[r].hint);
    }
    raw.push(["radio", await sh.run("radio")]); /* state, holders and cost of each radio, and the memory they need */
    if (skipped.length) for (const s of skipped) console.log(`[${id}] skipped: ${s}`);
    else for (const step of scn.steps) await runStep(step, link, sh, raw, values);
    appendFileSync(opts.out, "\n" + renderSection({ when, scenario: id, board, target, commit, skipped, values, perfTargets, raw }));
    console.log(`[${id}] ${skipped.length ? "skipped" : "measured"}; section appended to ${opts.out}`);
  }
  link.close();
}

if (import.meta.main) {
  main().then(() => process.exit(0), (e) => {
    console.error("error:", (e as Error).message);
    process.exit(1);
  });
}
