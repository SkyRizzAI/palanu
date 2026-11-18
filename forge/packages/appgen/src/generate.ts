// Built-in app table (T2.9): apps/apps.toml -> apps/manifests.gen.c. Pure apart from reading the two files; writing is done
// by cli.ts. Problems are errors (nothing is generated); the output is checked in and `--check` fails when it is stale.
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { parseToml, type Table } from "./toml.ts";

export const SOURCE = "apps/apps.toml";
export const OUTPUT = "apps/manifests.gen.c";
const PAPP_HEADER = "system/include/pal/papp.h";

// Manifest permission names (docs/PAPP.md) and the bits they set in system/include/pal/papp.h. "fs.home" and "fs.media"
// imply fs, as the parser does; the expression is emitted as written here.
export const CAP_BITS: Record<string, string> = {
  fs: "PAL_PERM_FS",
  "fs.home": "PAL_PERM_FS | PAL_PERM_FS_HOME",
  "fs.media": "PAL_PERM_FS | PAL_PERM_FS_MEDIA",
  net: "PAL_PERM_NET",
  "net.listen": "PAL_PERM_NET_LISTEN",
  "proc.spawn": "PAL_PERM_PROC",
  "crypto.keys": "PAL_PERM_CRYPTO_KEYS",
  "settings.read": "PAL_PERM_SETTINGS_READ",
  "settings.write": "PAL_PERM_SETTINGS_WRITE",
  notify: "PAL_PERM_NOTIFY",
  dev: "PAL_PERM_DEV",
  "svc.control": "PAL_PERM_SVC_CONTROL",
  link: "PAL_PERM_LINK",
};
const KINDS: Record<string, string> = { service: "PAL_KIND_SERVICE", tty: "PAL_KIND_TTY", gui: "PAL_KIND_GUI" };

export interface Generated {
  file: string; // the text of apps/manifests.gen.c
  problems: string[];
}

type Row = { id: string; name: string; version: string; api: number; kind: string; hidden: boolean; caps: string[]; stack_kb: number; mem_kb: number; fuel_m: number; icon: string; category: string; desktops: string };

const ID_RE = /^[a-z0-9_-]{1,23}$/;

function str(t: Table, key: string, where: string, max: number, dflt: string | undefined, problems: string[]): string {
  const v = t[key];
  if (v === undefined) {
    if (dflt === undefined) problems.push(`${where}: ${key} is required`);
    return dflt ?? "";
  }
  if (typeof v !== "string") {
    problems.push(`${where}: ${key} must be a string`);
    return "";
  }
  if (v.length > max) problems.push(`${where}: ${key} is longer than ${max} characters`);
  return v;
}

function num(t: Table, key: string, where: string, max: number, problems: string[]): number {
  const v = t[key];
  if (v === undefined) return 0;
  if (typeof v !== "number" || v < 0 || v > max) {
    problems.push(`${where}: ${key} must be a whole number from 0 to ${max}`);
    return 0;
  }
  return v;
}

function flag(t: Table, key: string, where: string, problems: string[]): boolean {
  const v = t[key];
  if (v === undefined) return false;
  if (typeof v !== "boolean") problems.push(`${where}: ${key} must be true or false`);
  return v === true;
}

function caps(t: Table, where: string, problems: string[]): string[] {
  const v = t.caps;
  if (v === undefined) return [];
  if (!Array.isArray(v)) {
    problems.push(`${where}: caps must be an array of names`);
    return [];
  }
  for (const c of v) if (!(c in CAP_BITS)) problems.push(`${where}: unknown capability "${c}"`);
  return v;
}

/* desktops = ["classic"]: the sessions an app belongs to (ADR-0010); none means every session. Stored as one comma-separated text. */
function desktops(t: Table, where: string, problems: string[]): string {
  const v = t.desktops;
  if (v === undefined) return "";
  if (!Array.isArray(v) || v.some((d) => typeof d !== "string" || !ID_RE.test(d))) {
    problems.push(`${where}: desktops must be an array of desktop ids`);
    return "";
  }
  const s = (v as string[]).join(",");
  if (s.length > 31) problems.push(`${where}: desktops is longer than 31 characters`);
  return s;
}

function kindOf(t: Table, where: string, dflt: string | undefined, problems: string[]): string {
  const k = str(t, "kind", where, 16, dflt, problems);
  if (k !== "" && !(k in KINDS)) problems.push(`${where}: kind must be service, tty or gui`);
  return k;
}

function apiOf(t: Table, where: string, problems: string[]): number {
  const a = t.api === undefined ? 2 : t.api;
  if (typeof a !== "number" || a < 1 || a > 2) problems.push(`${where}: api must be 1 or 2`);
  return typeof a === "number" ? a : 2;
}

export function generate(root: string): Generated {
  const problems: string[] = [];
  const src = join(root, SOURCE);
  if (!existsSync(src)) return { file: "", problems: [`${SOURCE} is missing`] };
  let doc: Table;
  try {
    doc = parseToml(readFileSync(src, "utf8"));
  } catch (e) {
    return { file: "", problems: [`${SOURCE}: ${(e as Error).message}`] };
  }

  // The bits the table names must exist in the header the device compiles against.
  const header = join(root, PAPP_HEADER);
  const defined = existsSync(header) ? new Set([...readFileSync(header, "utf8").matchAll(/#define\s+(PAL_PERM_\w+)/g)].map((m) => m[1])) : new Set<string>();
  for (const [name, expr] of Object.entries(CAP_BITS))
    for (const macro of expr.split("|").map((s) => s.trim()))
      if (!defined.has(macro)) problems.push(`${PAPP_HEADER}: ${macro} (capability ${name}) is not defined`);

  // [defaults.command]
  const defs = (doc.defaults as Table | undefined)?.command as Table | undefined;
  const cmd = { kind: "tty", hidden: true, api: 2 };
  if (defs) {
    cmd.kind = kindOf(defs, "[defaults.command]", "tty", problems);
    cmd.hidden = flag(defs, "hidden", "[defaults.command]", problems);
    cmd.api = apiOf(defs, "[defaults.command]", problems);
  }

  const rows: Row[] = [];
  const apps = (doc.app ?? []) as Table[];
  if (!Array.isArray(apps)) problems.push(`${SOURCE}: app must be [[app]] tables`);
  const seen = new Set<string>();
  apps.forEach((t, i) => {
    const id = str(t, "id", `[[app]] #${i + 1}`, 23, undefined, problems);
    const where = `[[app]] ${id || "#" + (i + 1)}`;
    if (id && !ID_RE.test(id)) problems.push(`${where}: id must be 1-23 characters of a-z 0-9 _ -`);
    if (seen.has(id)) problems.push(`${where}: duplicate id`);
    seen.add(id);
    rows.push({
      id,
      name: str(t, "name", where, 31, id, problems),
      version: str(t, "version", where, 15, "0.1", problems),
      api: apiOf(t, where, problems),
      kind: kindOf(t, where, undefined, problems),
      hidden: flag(t, "hidden", where, problems),
      caps: caps(t, where, problems),
      stack_kb: num(t, "stack_kb", where, 1024, problems),
      mem_kb: num(t, "mem_kb", where, 65536, problems),
      fuel_m: num(t, "fuel_m", where, 1000000, problems),
      icon: str(t, "icon", where, 23, "", problems),
      category: str(t, "category", where, 15, "", problems),
      desktops: desktops(t, where, problems),
    });
  });
  if (problems.length) return { file: "", problems };
  return { file: emit(rows, cmd), problems };
}

const q = (s: string) => JSON.stringify(s); // ASCII names only; JSON string syntax is valid C

function emit(rows: Row[], cmd: { kind: string; hidden: boolean; api: number }): string {
  const out: string[] = [];
  out.push("/* GENERATED by forge/packages/appgen from apps/apps.toml. Do not edit by hand.");
  out.push(" * Regenerate: bun forge/packages/appgen/src/cli.ts        Check: bun forge/packages/appgen/src/cli.ts --check */");
  out.push('#include "pal/papp.h"');
  out.push('#include "pal/registry.h"');
  out.push("");
  out.push("const pal_reg_builtin_t PAL_BUILTIN_APPS[] = {");
  for (const r of rows) {
    const caps = r.caps.length ? r.caps.map((c) => CAP_BITS[c]).join(" | ") : "0";
    out.push(
      `    {${q(r.id)}, ${q(r.name)}, ${q(r.version)}, ${r.api}, ${KINDS[r.kind]}, ${r.hidden}, ${caps}, ${r.stack_kb}, ${r.mem_kb}, ${r.fuel_m}, ${q(r.icon)}, ${q(r.category)}, ${q(r.desktops)}},`,
    );
  }
  out.push("};");
  out.push("const size_t PAL_BUILTIN_APPS_COUNT = sizeof(PAL_BUILTIN_APPS) / sizeof(PAL_BUILTIN_APPS[0]);");
  out.push("");
  out.push(`const pal_reg_defaults_t PAL_BUILTIN_COMMAND_DEFAULTS = {${KINDS[cmd.kind]}, ${cmd.hidden}, ${cmd.api}};`);
  return out.join("\n") + "\n";
}

// Problems plus staleness: what `--check` and `./pal check` report.
export function appgenCheck(root: string): string[] {
  const g = generate(root);
  if (g.problems.length) return g.problems;
  const out = join(root, OUTPUT);
  const disk = existsSync(out) ? readFileSync(out, "utf8") : "";
  return disk === g.file ? [] : [`${OUTPUT}: stale (run: bun forge/packages/appgen/src/cli.ts)`];
}
