// palanu app projects: app.toml, templates, the wasm build, and running an installed app on a device (T2.8).
// The CLI lives in palanu.ts; everything here is plain functions so bun test can reach it.
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readdirSync, writeFileSync, readFileSync } from "node:fs";
import { inflateRawSync } from "node:zlib";
import { tmpdir } from "node:os";
import { isAbsolute, join, normalize, resolve } from "node:path";
import { packPapp, validateManifest, type Manifest } from "../packages/papp/src/index.ts";

export const SDK_DIR = resolve(import.meta.dir, "../../sdk");
export const KINDS = ["tty", "gui", "service"] as const;
export type Kind = (typeof KINDS)[number];

// ---------------------------------------------------------------- TOML subset
// Scalars: basic and literal strings, integers (decimal, 0x hex, underscores), true/false. Arrays of scalars
// (may span lines, trailing comma ok). Tables: [name] and [a.b] (only the names are kept). Not supported, and
// rejected with the line number: dates, floats, inline tables, arrays of tables, multi-line strings.
export type TomlScalar = string | number | boolean;
export type TomlValue = TomlScalar | TomlScalar[] | TomlTable;
export interface TomlTable {
  [key: string]: TomlValue;
}

function stripComment(line: string): string {
  let quote = "";
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (quote) {
      if (c === "\\" && quote === '"') i++;
      else if (c === quote) quote = "";
    } else if (c === '"' || c === "'") quote = c;
    else if (c === "#") return line.slice(0, i);
  }
  return line;
}

/* Brackets still open in a value, outside strings. */
function openDepth(s: string): number {
  let depth = 0, quote = "";
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (quote) {
      if (c === "\\" && quote === '"') i++;
      else if (c === quote) quote = "";
    } else if (c === '"' || c === "'") quote = c;
    else if (c === "[") depth++;
    else if (c === "]") depth--;
  }
  return depth;
}

type Cursor = { s: string; i: number; where: string };

function skipWs(p: Cursor) {
  while (p.i < p.s.length && (p.s[p.i] === " " || p.s[p.i] === "\t")) p.i++;
}

function readString(p: Cursor, quote: string): string {
  p.i++; // opening quote
  let out = "";
  for (;;) {
    if (p.i >= p.s.length) throw new Error(`${p.where}: unterminated string`);
    const c = p.s[p.i++];
    if (c === quote) return out;
    if (quote === "'" || c !== "\\") {
      out += c;
      continue;
    }
    const e = p.s[p.i++];
    const simple: Record<string, string> = { '"': '"', "\\": "\\", n: "\n", t: "\t", r: "\r", b: "\b", f: "\f" };
    if (e in simple) out += simple[e];
    else if (e === "u" && /^[0-9a-fA-F]{4}$/.test(p.s.slice(p.i, p.i + 4))) {
      out += String.fromCodePoint(parseInt(p.s.slice(p.i, p.i + 4), 16));
      p.i += 4;
    } else throw new Error(`${p.where}: unsupported escape \\${e}`);
  }
}

function parseAt(p: Cursor): TomlValue {
  skipWs(p);
  const c = p.s[p.i];
  if (c === '"' || c === "'") return readString(p, c);
  if (c === "[") {
    p.i++;
    const arr: TomlScalar[] = [];
    for (;;) {
      skipWs(p);
      if (p.s[p.i] === "]") {
        p.i++;
        return arr;
      }
      const item = parseAt(p);
      if (typeof item === "object") throw new Error(`${p.where}: arrays must hold plain values`);
      arr.push(item);
      skipWs(p);
      if (p.s[p.i] === ",") p.i++;
      else if (p.s[p.i] !== "]") throw new Error(`${p.where}: expected , or ] in array`);
    }
  }
  const m = /^[^\s,\]]+/.exec(p.s.slice(p.i));
  if (!m) throw new Error(`${p.where}: missing value`);
  p.i += m[0].length;
  const tok = m[0];
  if (tok === "true" || tok === "false") return tok === "true";
  if (/^[+-]?\d[\d_]*$/.test(tok)) return Number(tok.replace(/_/g, ""));
  if (/^0x[0-9a-fA-F][0-9a-fA-F_]*$/.test(tok)) return parseInt(tok.slice(2).replace(/_/g, ""), 16);
  throw new Error(`${p.where}: unsupported value ${tok} (strings in double quotes, integers, booleans, arrays)`);
}

function parseValue(src: string, where: string): TomlValue {
  const p: Cursor = { s: src, i: 0, where };
  const v = parseAt(p);
  skipWs(p);
  if (p.i !== src.length) throw new Error(`${where}: unexpected text after the value`);
  return v;
}

export function parseToml(text: string): TomlTable {
  const root: TomlTable = {};
  let cur = root;
  const lines = text.split(/\r?\n/);
  for (let i = 0; i < lines.length; i++) {
    const where = `line ${i + 1}`;
    const line = stripComment(lines[i]).trim();
    if (!line) continue;
    if (line.startsWith("[[")) throw new Error(`${where}: arrays of tables are not supported`);
    if (line.startsWith("[")) {
      const m = /^\[\s*([A-Za-z0-9_-]+(?:\s*\.\s*[A-Za-z0-9_-]+)*)\s*\]$/.exec(line);
      if (!m) throw new Error(`${where}: bad table header`);
      cur = root;
      for (const part of m[1].split(".").map((x) => x.trim())) {
        const next = cur[part] ?? (cur[part] = {});
        if (typeof next !== "object" || Array.isArray(next)) throw new Error(`${where}: ${part} is not a table`);
        cur = next as TomlTable;
      }
      continue;
    }
    const eq = line.indexOf("=");
    if (eq < 0) throw new Error(`${where}: expected key = value`);
    const key = line.slice(0, eq).trim();
    if (!/^[A-Za-z0-9_-]+$/.test(key)) throw new Error(`${where}: bad key "${key}"`);
    let raw = line.slice(eq + 1).trim();
    while (openDepth(raw) > 0) {
      i++;
      if (i >= lines.length) throw new Error(`${where}: array is not closed`);
      raw += " " + stripComment(lines[i]).trim();
    }
    if (key in cur) throw new Error(`${where}: duplicate key ${key}`);
    cur[key] = parseValue(raw, where);
  }
  return root;
}

// ---------------------------------------------------------------- app.toml
export interface AppSpec {
  id: string;
  name: string;
  version: string;
  api: number;
  kind: Kind;
  sources: string[];
  caps: string[];
  stack_kb?: number;
  mem_kb?: number;
  fuel_m?: number;
  icon?: string;
  unknown: string[]; // top-level keys this version does not use (reported as warnings)
}

const KNOWN_KEYS = ["id", "name", "version", "api", "kind", "sources", "caps", "stack_kb", "mem_kb", "fuel_m", "icon"];

/* Checks an app.toml table and returns the typed spec. Throws with a message that names the key. */
export function appSpecFrom(t: TomlTable): AppSpec {
  const str = (k: string, dflt?: string): string => {
    const v = t[k];
    if (v === undefined) {
      if (dflt === undefined) throw new Error(`app.toml: ${k} is required`);
      return dflt;
    }
    if (typeof v !== "string") throw new Error(`app.toml: ${k} must be a string`);
    return v;
  };
  const int = (k: string, dflt?: number): number | undefined => {
    const v = t[k];
    if (v === undefined) return dflt;
    if (typeof v !== "number" || !Number.isInteger(v) || v < 0) throw new Error(`app.toml: ${k} must be a whole number, 0 or more`);
    return v;
  };
  const list = (k: string, dflt: string[]): string[] => {
    const v = t[k];
    if (v === undefined) return dflt;
    if (!Array.isArray(v) || v.some((x) => typeof x !== "string")) throw new Error(`app.toml: ${k} must be an array of strings`);
    return v as string[];
  };
  const id = str("id");
  if (!/^[a-z0-9_-]{1,23}$/.test(id)) throw new Error(`app.toml: id must be 1-23 characters of a-z 0-9 _ -`);
  const kind = str("kind", "tty");
  if (!(KINDS as readonly string[]).includes(kind)) throw new Error(`app.toml: kind must be one of ${KINDS.join(", ")}`);
  const sources = list("sources", ["main.c"]);
  if (sources.length === 0) throw new Error("app.toml: sources is empty");
  for (const s of sources) {
    if (isAbsolute(s) || normalize(s).startsWith("..") || !s.endsWith(".c")) throw new Error(`app.toml: source ${s} must be a .c file inside the project`);
  }
  const spec: AppSpec = {
    id,
    name: str("name", id),
    version: str("version", "0.1.0"),
    api: int("api", 2) as number,
    kind: kind as Kind,
    sources,
    caps: list("caps", []),
    stack_kb: int("stack_kb"),
    mem_kb: int("mem_kb"),
    fuel_m: int("fuel_m"),
    icon: t.icon === undefined ? undefined : str("icon"),
    unknown: Object.keys(t).filter((k) => !KNOWN_KEYS.includes(k) && typeof t[k] !== "object"),
  };
  return spec;
}

export function readAppSpec(file: string): AppSpec {
  if (!existsSync(file)) throw new Error(`no ${file}; run palanu build inside a project made by palanu new`);
  return appSpecFrom(parseToml(readFileSync(file, "utf8")));
}

/* The manifest that goes into the .papp (packPapp writes it as format 2; kind is part of it). */
export function manifestOf(spec: AppSpec): Manifest {
  return {
    id: spec.id,
    name: spec.name,
    ver: spec.version,
    api: spec.api,
    kind: spec.kind,
    perm: spec.caps as Manifest["perm"],
    mem: spec.mem_kb,
    stack: spec.stack_kb,
    fuel: spec.fuel_m || undefined,
  };
}

// ---------------------------------------------------------------- templates
export function templateFiles(kind: Kind, id: string, name: string): Record<string, string> {
  const toml = `# PalanuOS app manifest. \`palanu build\` (run it in this directory) reads it.
id = "${id}"
name = "${name}"
version = "0.1.0"
api = 2
kind = "${kind}"                 # tty | gui | service
sources = ["main.c"]
caps = []                        # fs, fs.home, fs.media, net, net.listen (see docs/API.md)
stack_kb = 16
mem_kb = 256                     # linear memory limit; malloc returns NULL beyond it
fuel_m = 0                       # 0: no instruction limit
icon = "icon.pvi"                # placeholder: not packed yet
`;
  const main: Record<Kind, string> = {
    tty: `/* ${name}: a terminal app with a menu, built on paltui (docs/TUI.md). */
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"
#define PALTUI_IMPLEMENTATION
#define PALTUI_WITH_PAL
#include "paltui/paltui.h"

static tui_t ui; /* paltui keeps its state in this struct; it is too big for the stack */
static const char *const items[] = {"Say hello", "Show uptime", "Quit"};
#define NITEMS ((int)(sizeof(items) / sizeof(items[0])))

void _start(void) {
    int sel = 0, cols = 80, rows = 24, quit = 0;
    char msg[64] = "Choose with Up/Down and Enter.";
    if (tui_open(&ui, tui_io_pal()) != 0) return;
    while (!quit && tui_frame(&ui, 1000) == TUI_RUN) {
        tui_size(&ui, &cols, &rows);
        tui_region(&ui, 2, 1, cols - 4, rows - 2);
        tui_title(&ui, "${name}");
        tui_gap(&ui, 1);
        int chose = tui_menu(&ui, "items", items, NITEMS, &sel);
        tui_gap(&ui, 1);
        tui_label(&ui, msg);
        tui_status(&ui, " Up/Down move   Enter choose   Esc quit");
        const tui_key_t *k = tui_key(&ui);
        if (k && (k->sym == TUI_K_ESC || (k->mods & TUI_MOD_CTRL && k->cp == 'c'))) quit = 1;
        tui_end(&ui);
        if (!chose || quit) continue;
        if (sel == 0) snprintf(msg, sizeof msg, "Hello from ${name}.");
        else if (sel == 1) snprintf(msg, sizeof msg, "Uptime: %lld ms", (long long)pal_now_ms());
        else quit = 1;
    }
    tui_close(&ui);
    printf("${name}: bye\\n");
}
`,
    gui: `/* ${name}: a desktop app (kind gui). The frame API (task T5.2) is not in the SDK yet, so this stub only prints.
 * When T5.2 lands, draw with the frame API and handle focus and quit events here. */
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"

void _start(void) {
    printf("${name}: this is a GUI app stub.\\n");
    printf("A GUI app needs the frame API of task T5.2, which is not in the SDK yet.\\n");
}
`,
    service: `/* ${name}: a service loop. Logs every second and ends when a stop is asked (Ctrl-C or kill: a QUIT event). */
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"

void _start(void) {
    int32_t events = pal_events();
    int ticks = 0;
    pal_log(PAL_LOG_INFO, "${name} started");
    for (;;) {
        if (events < 0) { /* no event handle: a plain sleep, and the service ends after a while */
            pal_sleep_ms(1000);
            if (++ticks >= 10) break;
            continue;
        }
        struct { int32_t h; uint16_t want, got; } rec = {events, PAL_POLL_IN, 0};
        int32_t n = pal_wait(&rec, sizeof rec, 1000);
        if (n < 0) break;
        if (n > 0) {
            uint32_t ev[4];
            while (pal_event_next(ev, sizeof ev, 0) == 16)
                if (ev[0] == PAL_EV_QUIT) {
                    printf("${name}: stop asked, ending after %d ticks\\n", ticks);
                    pal_log(PAL_LOG_INFO, "${name} stopped");
                    return;
                }
            continue;
        }
        ticks++;
        printf("${name}: tick %d\\n", ticks);
    }
}
`,
  };
  const readme = `# ${name}

A PalanuOS app (\`kind = "${kind}"\`), created with \`palanu new\`.

## Build

From this folder (needs clang with the wasm32 target and wasm-ld or rust-lld, see docs/DEVELOPER.md):

\`\`\`bash
bun <repo>/forge/tools/palanu.ts build
\`\`\`

The result is \`${id}.papp\`.

## Deploy and run

\`\`\`bash
palanu deploy ${id}.papp --run --tcp 192.168.1.20:7100     # or --serial /dev/cu.usbserial-10
\`\`\`

Without \`--run\` the app is only installed: type \`${id}\` in the board shell. \`--run\` runs it, shows its
output until it ends, then prints the exit code.

## Files

- \`app.toml\`: id, name, version, kind, permissions (\`caps\`), memory limit (\`mem_kb\`), stack (\`stack_kb\`).
- \`main.c\`: the app code. Only \`palanu.h\` and the libraries in \`sdk/\` may be used.
- \`palanu_libc.h\` (in \`sdk/\`): string, \`malloc\`, \`printf\`, \`qsort\` and more. One source file defines
  \`PALANU_LIBC_IMPLEMENTATION\`.
`;
  return { "main.c": main[kind], "app.toml": toml, "README.md": readme };
}

export function validateNewName(name: string): void {
  if (!/^[a-z0-9_-]{1,23}$/.test(name)) throw new Error("the name is also the app id: 1-23 characters of a-z 0-9 _ -");
}

/* palanu new NAME [--kind tty|gui|service]: creates NAME/ with main.c, app.toml, README.md. */
export function newProject(name: string, kind: string, cwd: string): string {
  validateNewName(name);
  if (!(KINDS as readonly string[]).includes(kind)) throw new Error(`--kind must be one of ${KINDS.join(", ")}`);
  const dir = resolve(cwd, name);
  if (existsSync(dir) && readdirSync(dir).length > 0) throw new Error(`${name}/ exists and is not empty`);
  mkdirSync(dir, { recursive: true });
  for (const [file, text] of Object.entries(templateFiles(kind as Kind, name, name))) writeFileSync(join(dir, file), text);
  return dir;
}

// ---------------------------------------------------------------- build
export function findWasmLd(): string[] {
  const w = spawnSync("which", ["wasm-ld"]);
  if (w.status === 0) return [w.stdout.toString().trim()];
  const base = join(process.env.HOME ?? "", ".rustup/toolchains");
  if (existsSync(base))
    for (const t of readdirSync(base)) {
      const lib = join(base, t, "lib/rustlib");
      if (!existsSync(lib)) continue;
      for (const h of readdirSync(lib)) {
        const p = join(lib, h, "bin/rust-lld");
        if (existsSync(p)) return [p, "-flavor", "wasm"];
      }
    }
  throw new Error("no wasm linker: install LLVM (wasm-ld) or rustup (rust-lld)");
}

/* Compiles C sources for wasm32 (freestanding, -I sdk) and links them into one module that exports _start. */
export function compileWasm(sources: string[], outWasm: string): void {
  const work = mkdtempSync(join(tmpdir(), "palanu-build-"));
  const objs: string[] = [];
  for (const [i, src] of sources.entries()) {
    const obj = join(work, `unit${i}.o`);
    // -I sdk: apps include the SDK by its path under sdk/ (e.g. "paltui/paltui.h", "palanu_libc.h")
    const cc = spawnSync("clang", ["--target=wasm32", "-mcpu=mvp", "-mbulk-memory", "-O2", "-ffreestanding", "-nostdlib", "-Wall", "-I", SDK_DIR, "-c", src, "-o", obj], { stdio: "inherit" });
    if (cc.status !== 0) throw new Error("compile failed");
    objs.push(obj);
  }
  const [ld, ...ldArgs] = findWasmLd();
  const l = spawnSync(ld, [...ldArgs, "--no-entry", "--export=_start", "--strip-all", "-o", outWasm, ...objs], { stdio: "inherit" });
  if (l.status !== 0) throw new Error("link failed (an undefined symbol from palanu_libc.h means no source file defines PALANU_LIBC_IMPLEMENTATION)");
}

/* palanu build in a project directory: app.toml -> <id>.papp (or -o). Returns the output path and size. */
export async function buildProject(dir: string, out?: string): Promise<{ path: string; bytes: number }> {
  const spec = readAppSpec(join(dir, "app.toml"));
  for (const w of spec.unknown) console.warn(`warning: app.toml: unknown key ${w} (ignored)`);
  const work = mkdtempSync(join(tmpdir(), "palanu-proj-"));
  const wasm = join(work, "app.wasm");
  compileWasm(spec.sources.map((s) => join(dir, s)), wasm);
  const m = manifestOf(spec);
  validateManifest(m);
  const papp = packPapp(new Uint8Array(await Bun.file(wasm).arrayBuffer()), m);
  const dest = out ?? join(dir, `${spec.id}.papp`);
  await Bun.write(dest, papp);
  return { path: dest, bytes: papp.length };
}

// ---------------------------------------------------------------- run on a device
/* The shell prints no exit status, so deploy --run ends its command line with `; echo __PLN_$?_END`. The typed
 * line shows `$?` literally, so only the output (digits) matches. */
export const EXIT_MARK = /__PLN_(-?\d+)_END/;

export function exitCodeIn(text: string): number | null {
  const m = EXIT_MARK.exec(text);
  return m ? Number(m[1]) : null;
}

export interface ShellLink {
  onTty: (d: Uint8Array) => void;
  onTtyClose: () => void;
  openShell(cols: number, rows: number): void;
  ttyWrite(d: Uint8Array): void;
  closeShell(): void;
}

/* The app id from a .papp's manifest: the name of its command and of /apps/<id>.papp. Format 1 (header, then the
 * CBOR manifest) and format 2 (section table; the manifest is section 1, deflated when flag bit 0 is set). The hash
 * is checked on the device, not here. */
export function pappId(bytes: Uint8Array): string {
  if (bytes.length < 20 || String.fromCharCode(...bytes.subarray(0, 4)) !== "PLNU") throw new Error("not a .papp file");
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let m: Uint8Array;
  if (bytes[4] === 1) {
    m = bytes.subarray(20, 20 + dv.getUint32(8, true));
  } else if (bytes[4] === 2) {
    const sections = dv.getUint16(8, true);
    const entry = [...Array(sections).keys()].map((i) => 10 + 14 * i).find((e) => bytes[e] === 1);
    if (entry === undefined) throw new Error("the .papp has no manifest section");
    const off = dv.getUint32(entry + 2, true), stored = dv.getUint32(entry + 6, true);
    const raw = bytes.subarray(off, off + stored);
    m = bytes[entry + 1] & 1 ? new Uint8Array(inflateRawSync(raw)) : raw;
  } else throw new Error(`unknown .papp format ${bytes[4]}`);
  let p = 0;
  const head = (): [number, number] => {
    if (p >= m.length) throw new Error("bad .papp manifest");
    const ib = m[p++], info = ib & 31;
    let n = info;
    if (info === 24) n = m[p++];
    else if (info === 25) { n = (m[p] << 8) | m[p + 1]; p += 2; }
    else if (info === 26) { n = ((m[p] << 24) | (m[p + 1] << 16) | (m[p + 2] << 8) | m[p + 3]) >>> 0; p += 4; }
    else if (info > 26) throw new Error("bad .papp manifest");
    return [ib >> 5, n];
  };
  const skip = (): void => {
    const [major, n] = head();
    if (major === 2 || major === 3) p += n;
    else if (major === 4) for (let i = 0; i < n; i++) skip();
    else if (major === 5) for (let i = 0; i < 2 * n; i++) skip();
  };
  const [major, count] = head();
  if (major !== 5) throw new Error("bad .papp manifest");
  for (let i = 0; i < count; i++) {
    const [kmajor, klen] = head();
    const key = kmajor === 3 ? new TextDecoder().decode(m.subarray(p, p + klen)) : "";
    p += kmajor === 3 ? klen : 0;
    if (key === "id") {
      const [vmajor, vlen] = head();
      if (vmajor !== 3) throw new Error("bad .papp manifest: id");
      return new TextDecoder().decode(m.subarray(p, p + vlen));
    }
    skip();
  }
  throw new Error("the .papp manifest has no id");
}

/* A tail that could still become the exit mark: held back until the next chunk decides. Only this tail is held, so
 * a menu's last bytes reach the terminal at once. */
const PARTIAL_MARK = /^(_|__|__P|__PL|__PLN|__PLN_|__PLN_-?\d*(_(E(N(D)?)?)?)?)$/;

function holdFrom(s: string): number {
  for (let i = Math.max(0, s.length - 12); i < s.length; i++) if (PARTIAL_MARK.test(s.slice(i))) return i;
  return s.length;
}

/* Runs a command line (the app id and its arguments) on a Link shell, streams its output to out, and resolves with the
 * exit code (or -1 when the shell closed first). The exit mark and the shell prompt after it are not streamed. Keys from stdin are forwarded
 * by the caller through ttyWrite. */
export function runOnShell(link: ShellLink, commandLine: string, out: (d: Uint8Array) => void, cols = 80, rows = 24, sleep = (ms: number) => Bun.sleep(ms)): Promise<number> {
  const dec = new TextDecoder(), enc = new TextEncoder();
  let pending = "";
  let finish: (code: number) => void = () => {};
  const done = new Promise<number>((r) => (finish = r));
  link.onTty = (d) => {
    pending += dec.decode(d, { stream: true });
    const m = EXIT_MARK.exec(pending);
    if (m) {
      out(enc.encode(pending.slice(0, m.index)));
      pending = "";
      finish(Number(m[1]));
      return;
    }
    const keep = holdFrom(pending);
    if (keep > 0) out(enc.encode(pending.slice(0, keep)));
    pending = pending.slice(keep);
  };
  link.onTtyClose = () => {
    if (pending) out(enc.encode(pending));
    pending = "";
    finish(-1);
  };
  link.openShell(cols, rows);
  return (async () => {
    await sleep(400); // the shell prints its prompt first
    link.ttyWrite(enc.encode(`${commandLine}; echo __PLN_$?_END\n`));
    const code = await done;
    link.closeShell();
    return code;
  })();
}
