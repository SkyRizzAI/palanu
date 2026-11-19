#!/usr/bin/env bun
// `./pal release`: cut a PalanuOS release. Nothing is pushed or published: it makes a commit, a local tag and the files.
//
//   ./pal release --version 0.9.1 [--board /dev/cu.usbserial-10] [--skip-golden] [--dry-run]
//   ./pal release --bump patch|minor|major
//
// Gates, in this order, each stopping the release at its first failure:
//   1. a clean git tree on the main branch (untracked files are ignored)
//   2. ./pal check (code rules, API ledger, generated files)
//   3. the host tests, and the golden images (--skip-golden for a dry run of the rest)
//   4. the RELEASE firmware build (PAL_RELEASE=1: developer mode off by default), its RAM budget (ramcheck), the image against the OTA slot
//   5. with --board PORT: the smoke tour on the real board (forge/tools/board_smoke.py)
// Then it writes VERSION, system/include/pal/version.h and a CHANGELOG.md section from the commit subjects since the last
// tag, rebuilds so the image carries the version, copies the files to dist/release/ with a SHA-256 and release.json, commits
// "Release X" and tags vX. The OTA package is the .bin: `palanu ota`, Web Forge > Device > Firmware update, or flash it.
import { copyFileSync, existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "../..");
const OTA_SLOT = 0x400000; /* partitions: ota_0 and ota_1 are 4 MB each */
const WARN_AT = 0.85, FAIL_AT = 0.95;

const args = process.argv.slice(2);
const flag = (n: string) => args.includes(n);
const opt = (n: string) => (args.includes(n) ? args[args.indexOf(n) + 1] : undefined);
const dry = flag("--dry-run");

function sh(cmd: string, opts: { cwd?: string; capture?: boolean } = {}): { code: number; out: string } {
  const p = Bun.spawnSync(["bash", "-c", cmd], { cwd: opts.cwd ?? ROOT, stdout: opts.capture ? "pipe" : "inherit", stderr: opts.capture ? "pipe" : "inherit" });
  return { code: p.exitCode ?? 1, out: opts.capture ? p.stdout.toString().trim() : "" };
}
const git = (c: string) => sh(`git ${c}`, { capture: true }).out;
const fail = (m: string): never => { console.error(`\nrelease: ${m}`); process.exit(1); };
const step = (m: string) => console.log(`\n== ${m}`);

/* ---- version ---- */

const current = readFileSync(join(ROOT, "VERSION"), "utf8").trim();
const SEMVER = /^(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.-]+)?$/;

function nextVersion(): string {
  const v = opt("--version");
  if (v) return SEMVER.test(v) ? v : fail(`"${v}" is not a semantic version (1.2.3 or 1.2.3-rc.1)`);
  const b = opt("--bump");
  const m = SEMVER.exec(current);
  if (!b || !m) return fail("give --version X.Y.Z or --bump patch|minor|major");
  let [maj, min, pat] = [Number(m[1]), Number(m[2]), Number(m[3])];
  if (b === "major") [maj, min, pat] = [maj + 1, 0, 0];
  else if (b === "minor") [min, pat] = [min + 1, 0];
  else if (b === "patch") pat += 1;
  else fail(`--bump takes patch, minor or major, not "${b}"`);
  return `${maj}.${min}.${pat}`;
}
const version = nextVersion();

/* ---- the gates ---- */

step(`release ${current} -> ${version}${dry ? " (dry run: no files, no commit, no tag)" : ""}`);

step("1. the git tree");
if (git("rev-parse --abbrev-ref HEAD") !== "main") fail("release from the main branch");
if (git("status --porcelain --untracked-files=no")) fail("the git tree has uncommitted changes: commit or discard them first");
if (git(`tag -l v${version}`)) fail(`the tag v${version} exists already`);
console.log("clean, on main");

step("2. ./pal check");
if (sh("bun forge/tools/pal.ts check").code) fail("./pal check failed");

step("3. host tests and golden images");
if (flag("--skip-golden")) console.log("golden images skipped (--skip-golden): this is not a release run");
if (sh("bun forge/tools/pal.ts test").code) fail("the host tests failed");
if (!flag("--skip-golden") && sh("bun forge/tools/pal.ts golden check").code) fail("a golden image changed: review it and `./pal golden update`, or fix the regression");

step("4. firmware build, RAM budget, image size");
if (sh("PAL_RELEASE=1 bun forge/tools/pal.ts build").code) fail("the firmware build (or its ramcheck) failed");
const BIN = join(ROOT, "targets/hhv2/build/palanuos.bin");
if (!existsSync(BIN)) fail("no firmware image: targets/hhv2/build/palanuos.bin");
const size = statSync(BIN).size, pct = size / OTA_SLOT;
console.log(`image ${(size / 1024).toFixed(0)} KB of the ${(OTA_SLOT / 1024).toFixed(0)} KB OTA slot (${(pct * 100).toFixed(0)}%)`);
if (pct > FAIL_AT) fail("the image does not leave room in the OTA slot");
if (pct > WARN_AT) console.log("warning: the image is above 85% of the OTA slot");

if (opt("--board")) {
  step("5. smoke tour on the board");
  /* the release image itself goes on the board first (same sources as the artifact; only the version string differs) */
  if (sh(`PAL_RELEASE=1 PAL_PORT=${opt("--board")} bun forge/tools/pal.ts flash`).code) fail("flashing the release image failed");
  if (sh(`bun forge/tools/pal.ts smoke ${opt("--board")}`).code) fail("the board smoke tour failed");
} else console.log("\n(no --board: the board was not exercised; pass --board PORT for a release run)");

if (dry) {
  console.log("\ndry run done: every gate passed");
  process.exit(0);
}

/* ---- the files ---- */

step("version, changelog");
const last = git("describe --tags --abbrev=0 --match 'v*'");
const range = last ? `${last}..HEAD` : "HEAD";
const subjects = git(`log ${range} --no-merges --pretty=%s`).split("\n").filter(Boolean);
const area = (s: string) => /^(Fix|fix)/.test(s) ? "Fixes" : /^(Web Forge|Terminal|Menus|Notification|UI scale|Settings|Legend|microSD|Shell|ssh|Wi-Fi)/i.test(s) ? "Features" : "Changes";
const groups: Record<string, string[]> = { Features: [], Fixes: [], Changes: [] };
for (const s of subjects) groups[area(s)].push(s);
const date = new Date().toISOString().slice(0, 10);
let entry = `## ${version} (${date})\n\n`;
for (const [g, items] of Object.entries(groups)) if (items.length) entry += `### ${g}\n\n${items.map((i) => `- ${i}`).join("\n")}\n\n`;
const clPath = join(ROOT, "CHANGELOG.md");
const old = existsSync(clPath) ? readFileSync(clPath, "utf8") : "# Changelog\n\nPalanuOS releases, newest first. `./pal release` writes each section from the commit subjects since the last tag.\n\n";
const head = old.split("\n## ")[0].replace(/\s*$/, "\n\n");
const rest = old.includes("\n## ") ? "## " + old.split("\n## ").slice(1).join("\n## ") : "";
/* A section for this version that is already there was written by hand (the first release is curated): keep it. */
const curated = new RegExp(`^## ${version.replace(/\./g, "\\.")}( |$)`, "m").test(old);
if (curated) console.log(`CHANGELOG.md already has a ${version} section: kept as written`);
else writeFileSync(clPath, head + entry + rest);
writeFileSync(join(ROOT, "VERSION"), version + "\n");
writeFileSync(join(ROOT, "system/include/pal/version.h"), readFileSync(join(ROOT, "system/include/pal/version.h"), "utf8").replace(/PAL_OS_VERSION "[^"]*"/, `PAL_OS_VERSION "${version}"`));
console.log(`${subjects.length} commits since ${last || "the start"}`);

step("rebuild with the version, web console, artifacts");
if (sh("PAL_RELEASE=1 bun forge/tools/pal.ts build").code) fail("the rebuild failed");
if (sh("bun forge/web/build.ts").code) fail("the web console build failed");
const out = join(ROOT, "dist/release");
mkdirSync(out, { recursive: true });
const name = `palanuos-${version}-hhv2.bin`;
copyFileSync(BIN, join(out, name));
copyFileSync(join(ROOT, "forge/web/dist/web-forge.html"), join(out, `web-forge-${version}.html`));
// the CJK and Hangul glyphs: a file for /data (docs/FONTS.md), too big for the firmware image
if (sh(`bun forge/tools/fontpack.ts file ${join(out, "unifont-cjk.pfnt")} assets/fonts/unifont/unifont-17.0.05.hex.gz assets/fonts/unifont/unifont_upper-17.0.05.hex.gz`).code) fail("packing the CJK font failed");
const sha = (f: string) => createHash("sha256").update(readFileSync(join(out, f))).digest("hex");
const files = [name, `web-forge-${version}.html`, "unifont-cjk.pfnt"];
writeFileSync(join(out, `SHA256SUMS-${version}`), files.map((f) => `${sha(f)}  ${f}`).join("\n") + "\n");
writeFileSync(join(out, `release-${version}.json`), JSON.stringify({
  version, date, built_from: git("rev-parse --short HEAD"), board: "hhv2", image: name, bytes: statSync(join(out, name)).size,
  ota_slot_bytes: OTA_SLOT, sha256: sha(name), tested_on_board: !!opt("--board"), golden: !flag("--skip-golden"),
}, null, 2) + "\n");
console.log(`dist/release/: ${files.join(", ")}`);

/* The catalogue offers the new image to devices (ADR-0016): a firmware line pointing at the release download, and index.txt.sig
 * when PALANU_RELEASE_KEY names the release key file (a device updates from an unsigned list only in developer mode). */
const repo = /github\.com[:/](.+?)(\.git)?$/.exec(git("remote get-url origin").trim())?.[1] ?? "SkyRizzAI/palanu";
const fwUrl = `https://github.com/${repo}/releases/download/v${version}/${name}`;
const relKey = process.env.PALANU_RELEASE_KEY;
if (sh(`bun forge/tools/store.ts index store --firmware hhv2=${join(out, name)}@${fwUrl}${relKey ? ` --sign ${relKey}` : ""}`).code) fail("the catalogue update failed");
if (!relKey) console.log("store/index.txt is not signed: set PALANU_RELEASE_KEY to sign it (docs/RELEASE.md)");

step("commit and tag (local; nothing is pushed)");
/* The commit only when the files changed: a version that was already written by hand (0.9.0) leaves nothing to commit, and the tag then goes on HEAD. */
if (sh(`git add VERSION CHANGELOG.md system/include/pal/version.h store/index.txt && (test ! -f store/index.txt.sig || git add store/index.txt.sig) && (git diff --cached --quiet || git commit -q -m "Release ${version}") && git tag -a v${version} -m "PalanuOS ${version}"`).code) fail("the commit or the tag failed");
console.log(`\nreleased ${version}: commit and tag v${version} are local. Push them yourself when you decide to publish.`);
