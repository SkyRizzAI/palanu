#!/usr/bin/env bun
// The app catalogue (docs/STORE.md, ADR-0015): builds the packages of the projects listed below and writes the list the device reads
// (system/src/store.c): a first line "palanu-store 1", then per package id, version, name, size, sha256, path, summary, by tabs.
//   bun forge/tools/store.ts build [OUTDIR] [options]   default OUTDIR: store/ (what the device fetches from GitHub by default)
//   bun forge/tools/store.ts index DIR [options]        only the list, for .papp files already in DIR
// Options (ADR-0016):
//   --firmware BOARD=FILE[@URL]  a "firmware:BOARD" line for PalanuOS (the version in system/include/pal/version.h): size and
//                                SHA-256 from FILE; the device downloads URL, or FILE copied next to the list when no URL is given
//   --sign KEYFILE               index.txt.sig: Ed25519 over the list's SHA-256 with a key from `palanu keygen`. A device trusts it
//                                when the public key is in /etc/trust or compiled in as a release key (system/src/trust.c)
// The packages are unsigned: a device installs them in developer mode, or after a release key signs them.
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { basename, join, resolve } from "node:path";
import { inflateRawSync } from "node:zlib";
import { buildProject, readAppSpec } from "./palanu_app.ts";
import { seedFromText, signDigest } from "../packages/papp/src/index.ts";

const ROOT = resolve(import.meta.dir, "../..");

/* The projects in the catalogue, and the line the device shows for each. */
const APPS: { dir: string; summary: string }[] = [
  { dir: "sdk/examples/hello-tty", summary: "The smallest terminal app: prints a greeting and its arguments" },
  { dir: "sdk/examples/net-client", summary: "TCP client: connect, send a line, print the reply" },
  { dir: "sdk/examples/devices", summary: "Lists the board's devices and blinks its LED" },
  { dir: "sdk/examples/hello-service", summary: "A background service that logs a line every few seconds" },
];

/* The manifest of a .papp (format 2): id, ver and name from its CBOR map. Enough of CBOR for the packer's output. */
function manifestOf(p: Uint8Array): { id: string; ver: string; name: string } {
  const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
  if (p[0] !== 0x50 || p[1] !== 0x4c || p[4] !== 2) throw new Error("not a format 2 .papp");
  const n = dv.getUint16(8, true);
  for (let i = 0; i < n; i++) {
    const e = 10 + 14 * i;
    if (p[e] !== 1) continue;
    const off = dv.getUint32(e + 2, true), len = dv.getUint32(e + 6, true);
    let m = p.subarray(off, off + len);
    if (p[e + 1] & 1) m = new Uint8Array(inflateRawSync(m));
    const out: Record<string, unknown> = {};
    let k = 0;
    const take = (): unknown => {
      const b = m[k++], major = b >> 5;
      let v = b & 31;
      if (v === 24) v = m[k++];
      else if (v === 25) v = (m[k++] << 8) | m[k++];
      else if (v === 26) v = ((m[k++] << 24) | (m[k++] << 16) | (m[k++] << 8) | m[k++]) >>> 0;
      if (major === 0) return v;
      if (major === 3) { const s = new TextDecoder().decode(m.subarray(k, k + v)); k += v; return s; }
      if (major === 4) return Array.from({ length: v }, take);
      if (major === 7) return v === 21;
      throw new Error("unexpected CBOR");
    };
    const b = m[k++];
    for (let j = 0; j < (b & 31); j++) { const key = take() as string; out[key] = take(); }
    return { id: String(out.id ?? ""), ver: String(out.ver ?? "1.0.0"), name: String(out.name ?? out.id ?? "") };
  }
  throw new Error("no manifest");
}

const argv = process.argv.slice(2);
const opts = (name: string) => argv.flatMap((a, i) => (a === name && argv[i + 1] ? [argv[i + 1]] : []));
const OS_VERSION = /PAL_OS_VERSION "([^"]+)"/.exec(readFileSync(join(ROOT, "system/include/pal/version.h"), "utf8"))![1];

function index(dir: string, summaries: Record<string, string>) {
  const lines = ["palanu-store 1"];
  for (const f of readdirSync(dir).filter((x) => x.endsWith(".papp")).sort()) {
    const p = new Uint8Array(readFileSync(join(dir, f)));
    const m = manifestOf(p);
    const sha = createHash("sha256").update(p).digest("hex");
    const clean = (s: string) => s.replace(/[\t\r\n]/g, " ");
    lines.push([m.id, m.ver, clean(m.name), p.length, sha, f, clean(summaries[m.id] ?? "")].join("\t"));
  }
  for (const spec of opts("--firmware")) {
    const m = /^([a-z0-9_-]+)=([^@]+)(?:@(.+))?$/.exec(spec);
    if (!m || !existsSync(m[2])) throw new Error(`--firmware BOARD=FILE[@URL]: ${spec}`);
    const [, board, file, url] = m;
    const img = readFileSync(file);
    let path = url;
    if (!path) {
      path = `palanuos-${OS_VERSION}-${board}.bin`;
      copyFileSync(file, join(dir, path));
    }
    const sha = createHash("sha256").update(img).digest("hex");
    lines.push([`firmware:${board}`, OS_VERSION, "PalanuOS", img.length, sha, path, `PalanuOS ${OS_VERSION} for ${board}`].join("\t"));
  }
  const text = lines.join("\n") + "\n";
  writeFileSync(join(dir, "index.txt"), text);
  console.log(`${join(dir, "index.txt")}: ${lines.length - 1} entries`);
  const key = opts("--sign")[0];
  if (key) {
    const digest = createHash("sha256").update(text).digest();
    writeFileSync(join(dir, "index.txt.sig"), signDigest(seedFromText(new Uint8Array(readFileSync(key))), digest));
    console.log(`${join(dir, "index.txt.sig")}: signed`);
  }
}

const [cmd, arg0] = argv;
const arg = arg0 && !arg0.startsWith("--") ? arg0 : undefined;
if (cmd === "build") {
  const out = resolve(arg ?? join(ROOT, "store"));
  mkdirSync(out, { recursive: true });
  const summaries: Record<string, string> = {};
  for (const a of APPS) {
    const dir = join(ROOT, a.dir);
    const spec = readAppSpec(join(dir, "app.toml"));
    const r = await buildProject(dir, join(out, `${spec.id}.papp`));
    summaries[spec.id] = a.summary;
    console.log(`${basename(r.path)}: ${r.bytes} bytes`);
  }
  index(out, summaries);
} else if (cmd === "index" && arg && existsSync(arg)) {
  /* the summaries the list already has are kept */
  const old = join(resolve(arg), "index.txt"), summaries: Record<string, string> = {};
  if (existsSync(old)) for (const l of readFileSync(old, "utf8").split("\n").slice(1)) { const f = l.split("\t"); if (f.length > 6) summaries[f[0]] = f[6]; }
  index(resolve(arg), summaries);
} else {
  console.error("usage: store.ts build [OUTDIR] | index DIR   [--firmware BOARD=FILE[@URL]] [--sign KEYFILE]");
  process.exit(2);
}
