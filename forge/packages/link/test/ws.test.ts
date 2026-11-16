// Palanu Link over WebSocket (system/src/link_ws.c), the transport Web Forge uses on Wi-Fi and LAN: the upgrade, PLP frames
// split across WebSocket messages, a remote shell and the screen mirror, a plain HTTP request refused, and the trust rule
// (untrusted without --link-open). Needs build/host/palanu-sim (./pal build host).
import { afterAll, beforeAll, expect, test } from "bun:test";
import { existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { CH, Decoder, encodeFrame, type Frame } from "../src/frame.ts";

const SIM = join(import.meta.dir, "../../../../build/host/palanu-sim");
const PORT = 7700 + Math.floor(Math.random() * 90);
const OPEN_PORT = PORT + 100; /* a second simulator, trusted links (--link-open) */
const te = new TextEncoder(), td = new TextDecoder();
const procs: ReturnType<typeof Bun.spawn>[] = [];

beforeAll(async () => {
  if (!existsSync(SIM)) throw new Error("build the simulator first: ./pal build host");
  for (const [port, open] of [[PORT, false], [OPEN_PORT, true]] as const) {
    const args = [SIM, "--ws", String(port), ...(open ? ["--link-open"] : []), "--wait", "120000"];
    procs.push(Bun.spawn(args, { env: { ...process.env, PALANU_DATA: mkdtempSync(join(tmpdir(), "palanu-ws-")) }, stdout: "ignore", stderr: "ignore" }));
  }
  await Bun.sleep(1500);
});

afterAll(() => { for (const p of procs) p.kill(); });

/** A WebSocket client speaking PLP; `split` sends each frame in single-byte messages, as a worst case. */
async function client(port: number, split = false) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}`);
  ws.binaryType = "arraybuffer";
  const frames: Frame[] = [];
  const waiters: { m: (f: Frame) => boolean; r: (f: Frame) => void }[] = [];
  const dec = new Decoder((f) => {
    const i = waiters.findIndex((w) => w.m(f));
    if (i >= 0) waiters.splice(i, 1)[0].r(f);
    else frames.push(f);
  });
  ws.onmessage = (e) => dec.push(new Uint8Array(e.data as ArrayBuffer));
  await new Promise<void>((res, rej) => { ws.onopen = () => res(); ws.onerror = () => rej(new Error("no websocket")); });
  const send = (chan: number, type: number, p?: Uint8Array) => {
    const f = encodeFrame(chan, type, p);
    if (split) for (const b of f) ws.send(Uint8Array.of(b));
    else ws.send(f);
  };
  const wait = (m: (f: Frame) => boolean, ms = 3000) => new Promise<Frame>((res, rej) => {
    const i = frames.findIndex(m);
    if (i >= 0) return res(frames.splice(i, 1)[0]);
    const t = setTimeout(() => rej(new Error("timeout")), ms);
    waiters.push({ m, r: (f) => { clearTimeout(t); res(f); } });
  });
  return { ws, send, wait, frames };
}

test("hello and ping over WebSocket; the link is untrusted without AUTH", async () => {
  const c = await client(PORT);
  c.send(CH.ctl, 1, te.encode("ws-test"));
  const info = JSON.parse(td.decode((await c.wait((f) => f.chan === 0 && f.type === 2)).payload));
  expect(info.os).toBe("PalanuOS");
  expect(info.trusted).toBe(false);
  c.send(CH.ctl, 3, Uint8Array.of(9, 8, 7, 6));
  expect(Array.from((await c.wait((f) => f.chan === 0 && f.type === 4)).payload)).toEqual([9, 8, 7, 6]);
  c.send(CH.tty, 1, Uint8Array.of(80, 0, 24, 0)); /* a shell before AUTH is refused (E_AUTH -23) */
  const err = await c.wait((f) => f.chan === CH.tty && f.type === 0x7f);
  expect((err.payload[0] << 24) >> 24).toBe(-23);
  c.ws.close();
});

test("frames split into one-byte messages are joined", async () => {
  await Bun.sleep(300);
  const c = await client(PORT, true);
  c.send(CH.ctl, 3, Uint8Array.of(1, 2, 3, 4));
  expect(Array.from((await c.wait((f) => f.chan === 0 && f.type === 4)).payload)).toEqual([1, 2, 3, 4]);
  c.ws.close();
});

test("a trusted WebSocket link runs a shell and mirrors the screen", async () => {
  const c = await client(OPEN_PORT);
  c.send(CH.ctl, 1, te.encode("ws-test"));
  expect(JSON.parse(td.decode((await c.wait((f) => f.chan === 0 && f.type === 2)).payload)).trusted).toBe(true);
  c.send(CH.tty, 1, Uint8Array.of(80, 0, 24, 0));
  await Bun.sleep(400);
  c.send(CH.tty, 2, te.encode("echo ws-shell-ok\r"));
  let out = "";
  for (let i = 0; i < 30 && !out.includes("ws-shell-ok\r\n"); i++) {
    try { out += td.decode((await c.wait((f) => f.chan === CH.tty && f.type === 2, 500)).payload); } catch { /* more */ }
  }
  expect(out).toContain("ws-shell-ok");
  c.send(CH.screen, 1, Uint8Array.of(1));
  const size = await c.wait((f) => f.chan === CH.screen && f.type === 5);
  expect(new DataView(size.payload.buffer, size.payload.byteOffset).getUint16(0, true)).toBe(320);
  await c.wait((f) => f.chan === CH.screen && f.type === 3, 3000); /* a RECT of the screen */
  c.ws.close();
});

test("a plain HTTP request gets 426, not a link", async () => {
  await Bun.sleep(300);
  const r = await fetch(`http://127.0.0.1:${PORT}/`).catch(() => null);
  expect(r?.status).toBe(426);
});
