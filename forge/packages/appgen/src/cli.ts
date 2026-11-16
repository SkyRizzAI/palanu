#!/usr/bin/env bun
// Generates apps/manifests.gen.c from apps/apps.toml.
//   bun forge/packages/appgen/src/cli.ts           write apps/manifests.gen.c
//   bun forge/packages/appgen/src/cli.ts --check   fail if the checked-in output is stale or the source has problems
import { resolve } from "node:path";
import { writeFileSync } from "node:fs";
import { generate, OUTPUT, appgenCheck } from "./generate.ts";

const ROOT = resolve(import.meta.dir, "../../../..");
const args = process.argv.slice(2);
if (args.some((a) => a !== "--check")) {
  console.error("usage: bun forge/packages/appgen/src/cli.ts [--check]");
  process.exit(2);
}
if (args.includes("--check")) {
  const problems = appgenCheck(ROOT);
  for (const p of problems) console.error(`appgen: ${p}`);
  if (problems.length) process.exit(1);
  console.log("appgen: built-in app table up to date");
  process.exit(0);
}
const g = generate(ROOT);
if (g.problems.length) {
  for (const p of g.problems) console.error(`error: ${p}`);
  console.error(`appgen: ${g.problems.length} problem(s); nothing written`);
  process.exit(1);
}
writeFileSync(resolve(ROOT, OUTPUT), g.file);
console.log(`wrote ${OUTPUT}`);
