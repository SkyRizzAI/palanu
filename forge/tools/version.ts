#!/usr/bin/env bun
// The next PalanuOS version and its changelog, from Conventional Commits (ADR-0022). Shared by `./pal release` (a release from
// this computer, with the board gates) and the release workflow (.github/workflows/release.yml, which opens the release PR).
//
//   bun forge/tools/version.ts next      JSON: the current version, the base, the bump, the next version ("" when nothing to release)
//   bun forge/tools/version.ts prepare   writes VERSION, system/include/pal/version.h and the CHANGELOG.md section; prints the version
//   bun forge/tools/version.ts notes V   prints the CHANGELOG.md section of V (the GitHub Release text)
//
// The rules, while PalanuOS is 0.x (docs/RELEASE.md): a breaking change ("!" or a BREAKING CHANGE footer) or a feat raises the
// minor, a fix or perf raises the patch, and docs, test, ci, build, chore, refactor and style alone make no release. From 1.0 a
// breaking change raises the major. The version is never typed by hand.
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

export const SEMVER = /^(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.-]+)?$/;
export type Bump = "major" | "minor" | "patch" | "none";
export interface Commit { hash: string; type: string; scope: string; breaking: boolean; summary: string; subject: string }

const CC = /^(\w+)(?:\(([^)]*)\))?(!)?:\s*(.+)$/;

/** One commit from its subject and body; a subject that is not a Conventional Commit has the type "other". */
export function parseCommit(hash: string, subject: string, body = ""): Commit {
  const m = CC.exec(subject.trim());
  const breaking = !!m?.[3] || /^BREAKING[ -]CHANGE:/m.test(body);
  /* a squash merge adds " (#12)": the PR number stays in the summary, where a reader finds the PR */
  return m ? { hash, type: m[1].toLowerCase(), scope: m[2] ?? "", breaking, summary: m[4].trim(), subject }
           : { hash, type: "other", scope: "", breaking, summary: subject.trim(), subject };
}

const RELEASING = new Set(["feat", "fix", "perf", "revert"]);

/** The bump a set of commits asks for, for the current version. Release commits themselves never count. */
export function bumpFor(commits: Commit[], current: string): Bump {
  const major = Number(SEMVER.exec(current)?.[1] ?? 0);
  let b: Bump = "none";
  const rank = { none: 0, patch: 1, minor: 2, major: 3 } as const;
  const up = (x: Bump) => { if (rank[x] > rank[b]) b = x; };
  for (const c of commits) {
    if (c.type === "chore" && c.scope === "release") continue;
    if (c.breaking) up(major >= 1 ? "major" : "minor");
    else if (c.type === "feat") up("minor");
    else if (RELEASING.has(c.type)) up("patch");
  }
  return b;
}

export function bumpVersion(current: string, b: Bump): string {
  const m = SEMVER.exec(current);
  if (!m) throw new Error(`VERSION "${current}" is not a semantic version`);
  let [maj, min, pat] = [Number(m[1]), Number(m[2]), Number(m[3])];
  const pre = !!m[4];
  if (b === "none") return "";
  /* a prerelease (0.10.0-rc.1) is finished by any release: it becomes 0.10.0 */
  if (pre) return `${maj}.${min}.${pat}`;
  if (b === "major") [maj, min, pat] = [maj + 1, 0, 0];
  else if (b === "minor") [min, pat] = [min + 1, 0];
  else pat += 1;
  return `${maj}.${min}.${pat}`;
}

const GROUPS: [string, (c: Commit) => boolean][] = [
  ["Breaking changes", (c) => c.breaking],
  ["Features", (c) => !c.breaking && c.type === "feat"],
  ["Fixes", (c) => !c.breaking && c.type === "fix"],
  ["Performance", (c) => !c.breaking && c.type === "perf"],
  ["Reverts", (c) => !c.breaking && c.type === "revert"],
];

/** The CHANGELOG.md section for a version: the releasing commits by group, each "scope: summary". */
export function changelogSection(version: string, date: string, commits: Commit[]): string {
  let out = `## ${version} (${date})\n\n`;
  for (const [title, pick] of GROUPS) {
    const items = commits.filter((c) => !(c.type === "chore" && c.scope === "release") && pick(c));
    if (items.length) out += `### ${title}\n\n${items.map((c) => `- ${c.scope ? `**${c.scope}**: ` : ""}${c.summary}`).join("\n")}\n\n`;
  }
  return out;
}

/** Puts a section on top of the changelog, under its title and introduction; a section already there for the version is kept. */
export function insertSection(changelog: string, version: string, section: string): string {
  if (new RegExp(`^## ${version.replace(/\./g, "\\.")}( |$)`, "m").test(changelog)) return changelog;
  const at = changelog.search(/^## /m);
  if (at < 0) return changelog.replace(/\s*$/, "\n\n") + section;
  return changelog.slice(0, at) + section + changelog.slice(at);
}

/** The section of one version, without its heading (the release notes). */
export function notesFor(changelog: string, version: string): string {
  const re = new RegExp(`^## ${version.replace(/\./g, "\\.")}[^\n]*\n([\\s\\S]*?)(?=^## |$(?![\\s\\S]))`, "m");
  return (re.exec(changelog)?.[1] ?? "").trim();
}

// ---------------------------------------------------------------- the repository

const ROOT = resolve(import.meta.dir, "../..");

function git(args: string[]): string {
  const p = Bun.spawnSync(["git", ...args], { cwd: ROOT, stdout: "pipe", stderr: "pipe" });
  return p.exitCode === 0 ? p.stdout.toString().trim() : "";
}

/** Where the commits of the next release start: the newest v* tag, or, before the first tag, the commit that last wrote VERSION
 *  (0.9.0 was written by hand and never tagged). */
export function base(): string {
  return git(["describe", "--tags", "--abbrev=0", "--match", "v*"]) || git(["log", "-1", "--format=%H", "--", "VERSION"]);
}

export function commitsSince(from: string): Commit[] {
  const SEP = "\x1e", FS = "\x1f";
  const out = git(["log", from ? `${from}..HEAD` : "HEAD", "--no-merges", `--format=%H${FS}%s${FS}%b${SEP}`]);
  return out.split(SEP).map((r) => r.trim()).filter(Boolean).map((r) => { const [h, s, b] = r.split(FS); return parseCommit(h, s, b ?? ""); });
}

export function plan() {
  const current = readFileSync(join(ROOT, "VERSION"), "utf8").trim();
  const from = base();
  const commits = commitsSince(from);
  const bump = bumpFor(commits, current);
  return { current, base: from, bump, next: bumpVersion(current, bump), commits };
}

export function writeVersion(version: string, commits: Commit[], date = new Date().toISOString().slice(0, 10)) {
  writeFileSync(join(ROOT, "VERSION"), version + "\n");
  const vh = join(ROOT, "system/include/pal/version.h");
  writeFileSync(vh, readFileSync(vh, "utf8").replace(/PAL_OS_VERSION "[^"]*"/, `PAL_OS_VERSION "${version}"`));
  const cl = join(ROOT, "CHANGELOG.md");
  const old = existsSync(cl) ? readFileSync(cl, "utf8") : "# Changelog\n\nPalanuOS releases, newest first.\n\n";
  writeFileSync(cl, insertSection(old, version, changelogSection(version, date, commits)));
}

if (import.meta.main) {
  const [cmd, arg] = process.argv.slice(2);
  if (cmd === "next") {
    const p = plan();
    console.log(JSON.stringify({ current: p.current, base: p.base, bump: p.bump, next: p.next, commits: p.commits.length }));
  } else if (cmd === "prepare") {
    const p = plan();
    if (p.next) writeVersion(p.next, p.commits);
    console.log(p.next);
  } else if (cmd === "notes" && arg) {
    console.log(notesFor(readFileSync(join(ROOT, "CHANGELOG.md"), "utf8"), arg));
  } else {
    console.error("usage: bun forge/tools/version.ts next | prepare | notes VERSION");
    process.exit(2);
  }
}
