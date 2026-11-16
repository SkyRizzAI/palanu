#!/usr/bin/env bun
// Generates the API outputs from api/palanu/*.h.
//   bun forge/packages/apigen/src/cli.ts           write sdk/palanu.h, system glue, api/symbols.csv, docs/API.md
//   bun forge/packages/apigen/src/cli.ts --check   regenerate in memory; fail if any output on disk differs
import { resolve } from "node:path";
import { generate, staleOutputs, writeOutputs } from "./generate.ts";

const ROOT = resolve(import.meta.dir, "../../../..");
const args = process.argv.slice(2);
const check = args.includes("--check");
if (args.some((a) => a !== "--check")) {
  console.error("usage: bun forge/packages/apigen/src/cli.ts [--check]");
  process.exit(2);
}

const g = generate(ROOT);
for (const w of g.warnings) console.warn(`warning: ${w}`);
if (g.problems.length) {
  for (const p of g.problems) console.error(`error: ${p}`);
  console.error(`apigen: ${g.problems.length} problem(s); nothing written`);
  process.exit(1);
}
if (check) {
  const stale = staleOutputs(ROOT, g.files);
  for (const p of stale) console.error(`stale: ${p} (run: bun forge/packages/apigen/src/cli.ts)`);
  if (stale.length) process.exit(1);
  console.log(`apigen: ${g.files.size} output(s) up to date`);
  process.exit(0);
}
for (const p of writeOutputs(ROOT, g.files)) console.log(`wrote ${p}`);
