#!/usr/bin/env bun
// Golden images: run palanu-sim on each scene and theme, and compare the screen with tests/golden/ref.
//   bun forge/tools/golden.ts check    compare every scene; write a diff PNG for each mismatch
//   bun forge/tools/golden.ts update   rewrite the references from the current build
//   bun forge/tools/golden.ts update home,lock   only the scenes whose names start with those prefixes
// Scenes and their steps live in forge/tools/golden-scenes.json. Output goes to tests/golden/out (ignored by git).
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { cpus } from "node:os";
import { join, resolve } from "node:path";
import { decodePNG, encodePNG, type Image, parsePPM } from "./png";

const ROOT = resolve(import.meta.dir, "../..");
const SIM = join(ROOT, "build/host/palanu-sim");
const SCENES = join(ROOT, "forge/tools/golden-scenes.json");
const REF = join(ROOT, "tests/golden/ref");
const OUT = join(ROOT, "tests/golden/out");
const CURSOR_HIDE = "\x1b[?25l"; // DECTCEM: the terminal cursor blinks, so it is hidden for reproducible screens

interface Scenes {
  start: string[];
  themes: Record<string, string[]>;
  leave: string[];
  scenes: Record<string, string[]>;
}

/* One scene in one theme: a fresh simulator process with its own data directory, so scenes can run side by side. */
async function runSim(scene: string, theme: string, sc: Scenes, steps: string[] = sc.scenes[scene]): Promise<Image> {
  const data = join(OUT, "data", `${scene}-${theme}`);
  rmSync(data, { recursive: true, force: true });
  mkdirSync(join(data, "home"), { recursive: true });
  writeFileSync(join(data, "home", "golden-cursor.esc"), CURSOR_HIDE);
  const shot = join(OUT, `${scene}-${theme}.ppm`);
  rmSync(shot, { force: true });
  const args = [
    ...sc.start,
    ...(sc.themes[theme] ?? []),
    ...sc.leave,
    ...steps,
    "--wait", "200",
    "--shot", shot,
  ];
  const p = Bun.spawn([SIM, ...args], {
    env: { ...process.env, PALANU_DATA: data },
    stdout: "ignore",
    stderr: "pipe",
  });
  const err = await new Response(p.stderr).text();
  const code = await p.exited;
  rmSync(data, { recursive: true, force: true });
  if (code !== 0 || !existsSync(shot)) {
    throw new Error(`palanu-sim failed for ${scene}/${theme} (exit ${code}): ${err.trim()}`);
  }
  return parsePPM(readFileSync(shot));
}

/* Runs the jobs with at most `limit` in flight and returns the results in job order. */
async function pool<T, R>(jobs: T[], limit: number, fn: (job: T) => Promise<R>): Promise<R[]> {
  const out: R[] = new Array(jobs.length);
  let next = 0;
  const worker = async () => {
    for (let i = next++; i < jobs.length; i = next++) out[i] = await fn(jobs[i]);
  };
  await Promise.all(Array.from({ length: Math.min(limit, jobs.length) }, worker));
  return out;
}

// Pixels that differ are drawn in red; the rest is the actual screen, dimmed, so the place is easy to see.
function diffImage(a: Image, b: Image): { count: number; img: Image } {
  const rgb = Buffer.alloc(a.w * a.h * 3);
  let count = 0;
  for (let i = 0; i < a.w * a.h; i++) {
    const o = i * 3;
    const same = a.rgb[o] === b.rgb[o] && a.rgb[o + 1] === b.rgb[o + 1] && a.rgb[o + 2] === b.rgb[o + 2];
    if (same) {
      rgb[o] = Math.round(a.rgb[o] * 0.3 + 180);
      rgb[o + 1] = Math.round(a.rgb[o + 1] * 0.3 + 180);
      rgb[o + 2] = Math.round(a.rgb[o + 2] * 0.3 + 180);
    } else {
      count++;
      rgb[o] = 255;
      rgb[o + 1] = 0;
      rgb[o + 2] = 0;
    }
  }
  return { count, img: { w: a.w, h: a.h, rgb } };
}

/* Paint the masked rectangles a fixed colour: they hold values that change by design. */
function applyMask(img: Image, rects: number[][] | undefined) {
  for (const [x, y, w, h] of rects ?? [])
    for (let yy = y; yy < Math.min(y + h, img.h); yy++)
      for (let xx = x; xx < Math.min(x + w, img.w); xx++) {
        const o = (yy * img.w + xx) * 3;
        img.rgb[o] = 128;
        img.rgb[o + 1] = 128;
        img.rgb[o + 2] = 128;
      }
}

async function main(): Promise<number> {
  const mode = process.argv[2];
  if (mode !== "check" && mode !== "update") {
    console.error("usage: bun forge/tools/golden.ts check | update");
    return 2;
  }
  if (!existsSync(SIM)) {
    console.error(`${SIM} not found; build it first (./pal golden does this)`);
    return 1;
  }
  const sc: Scenes = JSON.parse(readFileSync(SCENES, "utf8"));
  mkdirSync(OUT, { recursive: true });
  mkdirSync(REF, { recursive: true });
  // Optional third argument: scene name prefixes, comma separated (golden.ts update home,lock updates only those scenes).
  const only = process.argv[3] ? process.argv[3].split(",") : null;
  const jobs: { scene: string; theme: string }[] = [];
  for (const scene of Object.keys(sc.scenes)) {
    if (only && !only.some((p) => scene.startsWith(p))) continue;
    for (const theme of Object.keys(sc.themes)) jobs.push({ scene, theme });
  }
  // GOLDEN_JOBS sets how many simulators run at once (default: the CPU count, at most 8). The results do not depend on it.
  const limit = Math.max(1, Number(process.env.GOLDEN_JOBS) || Math.min(8, cpus().length));
  rmSync(join(OUT, "data"), { recursive: true, force: true });
  const lines: string[] = [];
  let failures = 0;
  await pool(jobs, limit, async ({ scene, theme }) => {
    const name = `${scene}-${theme}`;
    let actual: Image;
    try {
      actual = await runSim(scene, theme, sc);
    } catch (e) {
      lines.push(`FAIL ${name}: ${(e as Error).message}`);
      failures++;
      return;
    }
    applyMask(actual, (sc as any).masks?.[scene]);
    writeFileSync(join(OUT, `${name}.png`), encodePNG(actual));
    const refPath = join(REF, `${name}.png`);
    if (mode === "update") {
      writeFileSync(refPath, encodePNG(actual));
      lines.push(`updated ${name} (${actual.w}x${actual.h})`);
      return;
    }
    if (!existsSync(refPath)) {
      lines.push(`FAIL ${name}: no reference (run: ./pal golden update)`);
      failures++;
      return;
    }
    const ref = decodePNG(readFileSync(refPath));
    if (ref.w !== actual.w || ref.h !== actual.h) {
      lines.push(`FAIL ${name}: size ${actual.w}x${actual.h}, reference ${ref.w}x${ref.h}`);
      failures++;
      return;
    }
    const d = diffImage(actual, ref);
    if (d.count === 0) {
      lines.push(`ok   ${name}`);
      return;
    }
    const diffPath = join(OUT, `${name}.diff.png`);
    writeFileSync(diffPath, encodePNG(d.img));
    lines.push(`FAIL ${name}: ${d.count} of ${actual.w * actual.h} pixels differ (diff: ${diffPath})`);
    failures++;
  });
  console.log(lines.sort((x, y) => x.slice(5).localeCompare(y.slice(5))).join("\n"));
  if (mode === "update") console.log(`references written to ${REF}`);
  else console.log(`golden: ${failures} failure(s)`);
  return failures ? 1 : 0;
}

main().then((code) => process.exit(code));
