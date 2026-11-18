// .papp packer, format 2 (docs/PAPP.md). Format 1 is still read by the device; this writes format 2 only.
//   packPapp(wasm, manifest)                    unsigned, code deflated when that is smaller
//   packPapp(wasm, manifest, { sign: seed })    plus an Ed25519 signature over the SHA-256 (seed: 32 bytes)
import { createHash, createPrivateKey, createPublicKey, sign as edSign } from "node:crypto";
import { deflateRawSync } from "node:zlib";

export interface Manifest {
  id: string;
  name?: string;
  ver?: string;
  api?: number;
  perm?: Perm[];
  mem?: number; // KB of linear memory at start
  stack?: number; // KB
  fuel?: number; // million instructions before the watchdog stops the app (0 = none)
  kind?: "service" | "tty" | "gui";
  icon?: string; // icon id for the launcher
  category?: string; // launcher category
  hidden?: boolean; // not listed in the launcher
  desktops?: string; // the desktop sessions it belongs to, comma-separated ("classic"); absent = every session
}

export interface PackOptions {
  sign?: Uint8Array; // 32-byte Ed25519 seed; adds the "SIG0" trailer
  compress?: boolean; // default true: each section is stored deflated when that is smaller
  icon?: Uint8Array; // ICON section (opaque bytes, up to 16 KB)
  res?: Uint8Array; // RES section (opaque bytes, up to 256 KB)
}

export const PERMS = ["fs", "fs.home", "fs.media", "net", "net.listen", "proc.spawn", "proc", "crypto.keys", "settings.read", "settings.write", "notify", "svc.control", "link", "dev"] as const;
export type Perm = (typeof PERMS)[number];

// ---- minimal CBOR encoder (maps, arrays, text, unsigned, bool) ----
function head(major: number, n: number): number[] {
  if (n < 24) return [(major << 5) | n];
  if (n < 256) return [(major << 5) | 24, n];
  if (n < 65536) return [(major << 5) | 25, n >> 8, n & 255];
  return [(major << 5) | 26, (n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
}
function enc(v: unknown): number[] {
  if (typeof v === "number") return head(0, v);
  if (typeof v === "boolean") return [v ? 0xf5 : 0xf4];
  if (typeof v === "string") { const b = [...Buffer.from(v, "utf8")]; return [...head(3, b.length), ...b]; }
  if (Array.isArray(v)) return [...head(4, v.length), ...v.flatMap(enc)];
  if (v && typeof v === "object") {
    const e = Object.entries(v).filter(([, x]) => x !== undefined);
    return [...head(5, e.length), ...e.flatMap(([k, x]) => [...enc(k), ...enc(x)])];
  }
  throw new Error("cannot encode " + typeof v);
}

export function validateManifest(m: Manifest) {
  if (!/^[a-z0-9_-]{1,23}$/.test(m.id)) throw new Error("id must be 1-23 characters of a-z 0-9 _ -");
  for (const p of m.perm ?? []) if (!(PERMS as readonly string[]).includes(p)) throw new Error("unknown permission " + p);
  if (m.icon !== undefined && m.icon.length > 23) throw new Error("icon must be at most 23 characters");
  if (m.category !== undefined && m.category.length > 15) throw new Error("category must be at most 15 characters");
  if (m.desktops !== undefined && !/^[a-z0-9_-]+(,[a-z0-9_-]+)*$/.test(m.desktops)) throw new Error("desktops must be desktop ids separated by commas");
  if (m.desktops !== undefined && m.desktops.length > 31) throw new Error("desktops must be at most 31 characters");
}

// ---- Ed25519 keys: a 32-byte seed in a file (hex text or raw bytes) ----
const PKCS8_ED25519 = Buffer.from("302e020100300506032b657004220420", "hex");
function privateKey(seed: Uint8Array) {
  if (seed.length !== 32) throw new Error("an Ed25519 seed is 32 bytes");
  return createPrivateKey({ key: Buffer.concat([PKCS8_ED25519, Buffer.from(seed)]), format: "der", type: "pkcs8" });
}

/** The public key of a seed, as 64 hex digits (the form /etc/trust accepts). */
export function publicKeyHex(seed: Uint8Array): string {
  const spki = createPublicKey(privateKey(seed)).export({ format: "der", type: "spki" }) as Buffer;
  return spki.subarray(spki.length - 32).toString("hex");
}

/** Reads a key file: 64 hex digits, or 32 raw bytes. */
export function seedFromText(bytes: Uint8Array): Uint8Array {
  if (bytes.length === 32) return bytes;
  const hex = Buffer.from(bytes).toString("utf8").trim();
  if (!/^[0-9a-fA-F]{64}$/.test(hex)) throw new Error("a key file holds 64 hex digits or 32 raw bytes");
  return Uint8Array.from(Buffer.from(hex, "hex"));
}

function sha256(b: Uint8Array): Buffer {
  return createHash("sha256").update(b).digest();
}

/** Packs a WebAssembly module and its manifest into a format 2 package. */
export function packPapp(wasm: Uint8Array, m: Manifest, opts: PackOptions = {}): Uint8Array {
  validateManifest(m);
  if (wasm.length < 8 || wasm[0] !== 0 || wasm[1] !== 0x61 || wasm[2] !== 0x73 || wasm[3] !== 0x6d) throw new Error("not a WebAssembly module");
  if (wasm.length > 1024 * 1024) throw new Error("module larger than 1 MB");
  const manifest = Uint8Array.from(enc({ api: 2, kind: "tty", ...m }));
  if (manifest.length > 4096) throw new Error("manifest larger than 4 KB");
  const compress = opts.compress !== false;
  const sections: { type: number; raw: Uint8Array }[] = [
    { type: 1, raw: manifest },
    { type: 2, raw: wasm },
  ];
  if (opts.icon) sections.push({ type: 3, raw: opts.icon });
  if (opts.res) sections.push({ type: 4, raw: opts.res });

  const built = sections.map((s) => {
    const z = compress ? deflateRawSync(s.raw, { level: 9 }) : null;
    const deflated = z !== null && z.length < s.raw.length;
    return { type: s.type, flags: deflated ? 1 : 0, raw: s.raw.length, stored: deflated ? z! : s.raw };
  });
  const signed = opts.sign !== undefined;
  const hlen = 10 + 14 * built.length;
  const parts: Buffer[] = [];
  const header = Buffer.alloc(hlen);
  header.write("PLNU", 0, "latin1");
  header[4] = 2; // format
  header[5] = signed ? 2 : 0; // flags: bit1 = signed
  header.writeUInt16LE(hlen, 6);
  header.writeUInt16LE(built.length, 8);
  let off = hlen;
  built.forEach((s, i) => {
    const e = 10 + 14 * i;
    header[e] = s.type;
    header[e + 1] = s.flags;
    header.writeUInt32LE(off, e + 2);
    header.writeUInt32LE(s.stored.length, e + 6);
    header.writeUInt32LE(s.raw, e + 10);
    parts.push(Buffer.from(s.stored));
    off += s.stored.length;
  });
  const body = Buffer.concat([header, ...parts]);
  const digest = sha256(body);
  const tail = [digest];
  if (signed) tail.push(Buffer.from("SIG0", "latin1"), edSign(null, digest, privateKey(opts.sign!)));
  return Buffer.concat([body, ...tail]);
}
