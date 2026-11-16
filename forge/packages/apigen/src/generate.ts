// Generates every API output from api/palanu/*.h. Pure apart from reading the tree; writeOutputs is the only writer.
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { emitDocs } from "./emit_docs.ts";
import { emitGlue, emitImplHeader } from "./emit_glue.ts";
import { emitSdk } from "./emit_sdk.ts";
import { ledgerRows, renderLedger } from "./ledger.ts";
import { type ModuleDef } from "./model.ts";
import { parseModule } from "./parse.ts";
import { lex } from "./lex.ts";

export const OUTPUTS = {
  sdk: "sdk/palanu.h",
  glue: "system/src/api_wasm_glue.c",
  impl: "system/src/api_impl.h",
  ledger: "api/symbols.csv",
  docs: "docs/API.md",
};

export interface Generated {
  files: Map<string, string>; // repo-relative path -> text; empty when there are problems
  problems: string[]; // errors: nothing is written
  warnings: string[];
}

const GENERATOR_CMD = "bun forge/packages/apigen/src/cli.ts";

function listHeaders(dir: string): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((f) => f.endsWith(".h") && f !== "annot.h")
    .sort();
}

// The API level the build targets (api/palanu/annot.h) and the one the firmware reports (papp.h) must agree.
function levelOf(root: string, problems: string[]): number {
  const annot = join(root, "api/palanu/annot.h");
  const m = existsSync(annot) ? /#define\s+PAL_API_LEVEL\s+(\d+)/.exec(readFileSync(annot, "utf8")) : null;
  if (!m) {
    problems.push("api/palanu/annot.h: #define PAL_API_LEVEL <n> is missing");
    return 0;
  }
  const papp = join(root, "system/include/pal/papp.h");
  const pm = existsSync(papp) ? /#define\s+PAL_PAPP_API\s+(\d+)/.exec(readFileSync(papp, "utf8")) : null;
  if (pm && Number(pm[1]) !== Number(m[1])) {
    problems.push(`system/include/pal/papp.h: PAL_PAPP_API ${pm[1]} differs from PAL_API_LEVEL ${m[1]} in api/palanu/annot.h`);
  }
  return Number(m[1]);
}

// Identifiers the system and the built-in apps declare: an SDK name that equals one of them would clash with it if both
// headers met in one translation unit. Maps each identifier to the header that has it.
function systemIdents(root: string): Map<string, string> {
  const files = [join(root, "apps/libapp.h")];
  const pal = join(root, "system/include/pal");
  if (existsSync(pal)) for (const f of readdirSync(pal).sort()) if (f.endsWith(".h")) files.push(join(pal, f));
  const seen = new Map<string, string>();
  for (const f of files) {
    if (!existsSync(f)) continue;
    const rel = f.slice(root.length + 1);
    for (const id of lex(readFileSync(f, "utf8"), rel).idents) if (!seen.has(id)) seen.set(id, rel);
  }
  return seen;
}

export function generate(root: string): Generated {
  const problems: string[] = [];
  const warnings: string[] = [];
  const level = levelOf(root, problems);
  const mods: ModuleDef[] = [];
  const dir = join(root, "api/palanu");
  for (const h of listHeaders(dir)) {
    const rel = `api/palanu/${h}`;
    const parsed = parseModule(rel, h.replace(/\.h$/, ""), readFileSync(join(dir, h), "utf8"), level);
    problems.push(...parsed.problems);
    mods.push(parsed.mod);
  }

  // Names are unique across modules: C names (functions, aliases, constants) and wasm import names.
  const cnames = new Map<string, string>();
  const wasmNames = new Map<string, string>();
  const claim = (map: Map<string, string>, name: string, where: string, what: string) => {
    const prev = map.get(name);
    if (prev) problems.push(`${where}: ${what} "${name}" is already defined at ${prev}`);
    else map.set(name, where);
  };
  for (const m of mods) {
    for (const f of m.funcs) {
      claim(cnames, f.cname, `${f.file}:${f.line}`, "function");
      if (f.alias) claim(cnames, f.alias, `${f.file}:${f.line}`, "alias");
      claim(wasmNames, f.wasm, `${f.file}:${f.line}`, "import name");
    }
    for (const c of m.consts) claim(cnames, c.name, `${c.file}:${c.line}`, "constant");
  }

  // Collisions with the system: a new public name may not equal an identifier the system declares. Old names kept as
  // SDK aliases only warn, since they never meet the system headers in one translation unit.
  const sys = systemIdents(root);
  for (const m of mods) {
    for (const f of m.funcs) {
      if (f.removed !== undefined) continue;
      const hit = sys.get(f.cname);
      if (hit) problems.push(`${f.file}:${f.line}: collision: ${f.cname} is already declared in ${hit}; choose another name`);
      if (f.alias && sys.has(f.alias)) warnings.push(`${f.file}:${f.line}: old name ${f.alias} is also declared in ${sys.get(f.alias)} (SDK alias only)`);
    }
    for (const c of m.consts) {
      const hit = sys.get(c.name);
      if (hit) problems.push(`${c.file}:${c.line}: collision: constant ${c.name} is already declared in ${hit}; choose another name`);
    }
  }
  if (problems.length) return { files: new Map(), problems, warnings };

  const files = new Map<string, string>();
  files.set(OUTPUTS.sdk, emitSdk(mods, level));
  files.set(OUTPUTS.glue, emitGlue(mods));
  files.set(OUTPUTS.impl, emitImplHeader(mods));
  files.set(OUTPUTS.ledger, renderLedger(ledgerRows(mods)));
  files.set(OUTPUTS.docs, emitDocs(mods, level));
  return { files, problems, warnings };
}

// Paths in `files` that differ from the disk (missing counts as different). Empty when everything is in sync.
export function staleOutputs(root: string, files: Map<string, string>): string[] {
  const stale: string[] = [];
  for (const [path, text] of files) {
    const abs = join(root, path);
    if (!existsSync(abs) || readFileSync(abs, "utf8") !== text) stale.push(path);
  }
  return stale;
}

export function writeOutputs(root: string, files: Map<string, string>): string[] {
  for (const [path, text] of files) {
    const abs = join(root, path);
    mkdirSync(dirname(abs), { recursive: true });
    writeFileSync(abs, text);
  }
  return [...files.keys()];
}

/* For `./pal check`: every problem in the definitions, plus one error per output that is out of date. Warnings are
 * returned apart, so the check can count them. */
export function apiCheckOutputs(root: string): { errors: string[]; warnings: string[] } {
  const g = generate(root);
  if (g.problems.length) return { errors: g.problems, warnings: g.warnings };
  const errors = staleOutputs(root, g.files).map((p) => `api: ${p} is out of date (run: ${GENERATOR_CMD})`);
  return { errors, warnings: g.warnings };
}
