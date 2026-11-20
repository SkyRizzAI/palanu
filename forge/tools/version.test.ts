// The release rules of forge/tools/version.ts (ADR-0022): which commits raise which part, the changelog and the notes.
import { expect, test } from "bun:test";
import { bumpFor, bumpVersion, changelogSection, insertSection, notesFor, parseCommit } from "./version.ts";

const c = (s: string, body = "") => parseCommit("h", s, body);

test("a commit is parsed by type, scope and breaking mark", () => {
  expect(c("feat(web): one page")).toMatchObject({ type: "feat", scope: "web", breaking: false, summary: "one page" });
  expect(c("fix!: drop the old flag")).toMatchObject({ type: "fix", breaking: true });
  expect(c("refactor(link): x", "BREAKING CHANGE: the frame changed")).toMatchObject({ breaking: true });
  expect(c("Release 0.9.0").type).toBe("other");
});

test("0.x: feat and breaking raise the minor, fix and perf the patch, the rest nothing", () => {
  expect(bumpFor([c("docs: x"), c("test: y"), c("ci: z"), c("chore: w")], "0.9.0")).toBe("none");
  expect(bumpFor([c("fix: a"), c("perf: b")], "0.9.0")).toBe("patch");
  expect(bumpFor([c("fix: a"), c("feat: b")], "0.9.0")).toBe("minor");
  expect(bumpFor([c("feat!: b")], "0.9.0")).toBe("minor");
  expect(bumpFor([c("feat!: b")], "1.2.3")).toBe("major");
  expect(bumpFor([c("chore(release): v0.10.0")], "0.9.0")).toBe("none");
});

test("a bump makes the next version; a prerelease is finished by any release", () => {
  expect(bumpVersion("0.9.0", "minor")).toBe("0.10.0");
  expect(bumpVersion("0.9.3", "patch")).toBe("0.9.4");
  expect(bumpVersion("1.4.2", "major")).toBe("2.0.0");
  expect(bumpVersion("0.10.0-rc.1", "patch")).toBe("0.10.0");
  expect(bumpVersion("0.9.0", "none")).toBe("");
});

test("the changelog groups the releasing commits and keeps a section written by hand", () => {
  const s = changelogSection("0.10.0", "2026-11-20", [c("feat(web): one page"), c("fix: a bug"), c("docs: words"), c("feat(api)!: renamed")]);
  expect(s).toBe("## 0.10.0 (2026-11-20)\n\n### Breaking changes\n\n- **api**: renamed\n\n### Features\n\n- **web**: one page\n\n### Fixes\n\n- a bug\n\n");
  const log = "# Changelog\n\nIntro.\n\n## 0.9.0 (2026-11-14)\n\nFirst.\n";
  const next = insertSection(log, "0.10.0", s);
  expect(next.startsWith("# Changelog\n\nIntro.\n\n## 0.10.0")).toBe(true);
  expect(next).toContain("## 0.9.0 (2026-11-14)");
  expect(insertSection(next, "0.10.0", "## 0.10.0 (other)\n")).toBe(next);
  expect(notesFor(next, "0.9.0")).toBe("First.");
  expect(notesFor(next, "0.10.0")).toContain("- **web**: one page");
});
