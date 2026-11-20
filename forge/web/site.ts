#!/usr/bin/env bun
// Assembles the GitHub Pages site (ADR-0022): Web Forge, the app catalogue, and one simulator per version, all on one origin so the
// page reads them without CORS (GitHub's release downloads send no CORS header; a page cannot fetch them).
//
//   bun forge/web/site.ts --out _site [--releases DIR] [--commit SHA] [--repo OWNER/NAME]
//
//   _site/index.html, coi-sw.js, flash/    Web Forge (forge/web/dist, from build.ts); coi-sw.js adds the isolation Pages cannot send
//   _site/store/                           the catalogue (store/)
//   _site/sim/dev/                         the simulator built from this commit of main
//   _site/sim/<version>/                   each release's simulator, from DIR/<tag>/ (palanu-web.js, palanu-web.wasm, release.json)
//   _site/firmware.json                    what the page offers: { dev, releases[] }, newest release first
//
// DIR/releases.json is `gh release list --json tagName,publishedAt,isPrerelease,isDraft`; the release workflow writes the rest.
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "../..");
const DIST = join(import.meta.dir, "dist");
const args = process.argv.slice(2);
const opt = (n: string, d = "") => (args.includes(n) ? args[args.indexOf(n) + 1] : d);
const out = resolve(opt("--out", "_site"));
const relDir = opt("--releases") ? resolve(opt("--releases")) : "";
const commit = opt("--commit", process.env.GITHUB_SHA ?? "");
const repo = opt("--repo", process.env.GITHUB_REPOSITORY ?? "SkyRizzAI/palanu");
const SIM = ["palanu-web.js", "palanu-web.wasm"];

const fail = (m: string): never => { console.error(`site: ${m}`); process.exit(1); };
const sha256 = (f: string) => createHash("sha256").update(readFileSync(f)).digest("hex");

if (!existsSync(join(DIST, "index.html"))) fail("forge/web/dist/index.html missing: run bun forge/web/build.ts first");
for (const f of SIM) if (!existsSync(join(DIST, f))) fail(`forge/web/dist/${f} missing: build the system for the browser first (docs/SIMULATOR.md)`);

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
copyFileSync(join(DIST, "index.html"), join(out, "index.html"));
copyFileSync(join(DIST, "coi-sw.js"), join(out, "coi-sw.js"));
mkdirSync(join(out, "flash"), { recursive: true });
copyFileSync(join(DIST, "flash/index.html"), join(out, "flash/index.html")); /* no firmware image yet: the page says so */
if (existsSync(join(ROOT, "store"))) cpSync(join(ROOT, "store"), join(out, "store"), { recursive: true });
writeFileSync(join(out, ".nojekyll"), ""); /* serve every file as it is */

interface Build { version: string; path: string; date: string; sha256: Record<string, string>; signed: false }
function place(from: string, dir: string, version: string, date: string): Build {
  mkdirSync(join(out, dir), { recursive: true });
  const sums: Record<string, string> = {};
  for (const f of SIM) { copyFileSync(join(from, f), join(out, dir, f)); sums[f] = sha256(join(from, f)); }
  return { version, path: `${dir}/`, date, sha256: sums, signed: false };
}

const VERSION = readFileSync(join(ROOT, "VERSION"), "utf8").trim();
const dev = { ...place(DIST, "sim/dev", VERSION, new Date().toISOString()), commit: commit.slice(0, 12) };

const releases: (Build & { tag: string; prerelease: boolean; notes: string })[] = [];
if (relDir && existsSync(join(relDir, "releases.json"))) {
  const list = JSON.parse(readFileSync(join(relDir, "releases.json"), "utf8")) as { tagName: string; publishedAt: string; isPrerelease: boolean; isDraft: boolean }[];
  for (const r of list) {
    const from = join(relDir, r.tagName);
    if (r.isDraft || !SIM.every((f) => existsSync(join(from, f)))) { console.warn(`site: ${r.tagName} has no simulator, skipped`); continue; }
    const version = r.tagName.replace(/^v/, "");
    releases.push({ ...place(from, `sim/${version}`, version, r.publishedAt), tag: r.tagName, prerelease: r.isPrerelease, notes: `https://github.com/${repo}/releases/tag/${r.tagName}` });
  }
}
releases.sort((a, b) => b.date.localeCompare(a.date));

writeFileSync(join(out, "firmware.json"), JSON.stringify({ schema: 1, repo, generated: new Date().toISOString(), dev, releases }, null, 2) + "\n");
const size = (d: string): number => readdirSync(d, { withFileTypes: true }).reduce((n, e) => n + (e.isDirectory() ? size(join(d, e.name)) : readFileSync(join(d, e.name)).length), 0);
console.log(`site: ${out} (${(size(out) / 1048576).toFixed(1)} MB): dev ${VERSION}${commit ? ` @${commit.slice(0, 7)}` : ""}, ${releases.length} release(s)${releases.length ? `: ${releases.map((r) => r.version).join(", ")}` : ""}`);
