// Runs the simulator with the TCP link and drives it like a real client. Needs build/host/palanu-sim (./pal build host).
import { afterAll, beforeAll, expect, test } from "bun:test";
import { existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Decoder, encodeFrame, Link, connectTcp, crc16 } from "../src/index.ts";

const SIM = join(import.meta.dir, "../../../../build/host/palanu-sim");
const PORT = 7800 + Math.floor(Math.random() * 100);
const T0 = Date.now();
let proc: ReturnType<typeof Bun.spawn> | undefined;

beforeAll(async () => {
  if (!existsSync(SIM)) throw new Error("build the simulator first: ./pal build host");
  const data = mkdtempSync(join(tmpdir(), "palanu-"));
  proc = Bun.spawn([SIM, "--link", String(PORT), "--link-open", "--wait", "60000"], { env: { ...process.env, PALANU_DATA: data }, stdout: process.env.PALANU_TEST_VERBOSE ? "inherit" : "ignore", stderr: process.env.PALANU_TEST_VERBOSE ? "inherit" : "ignore" });
  proc.exited.then((c) => { if (process.env.PALANU_TEST_VERBOSE) console.error("simulator exited with", c, "after", Date.now() - T0, "ms"); });
  for (let i = 0; i < 50; i++) { try { (await connectTcp("127.0.0.1", PORT)).close(); return; } catch { await Bun.sleep(100); } }
  throw new Error("simulator did not start");
});
afterAll(() => { if (process.env.PALANU_TEST_VERBOSE) console.error("teardown at", Date.now() - T0); proc?.kill(); });

const open = async () => { const l = new Link(await connectTcp("127.0.0.1", PORT)); await l.hello(); return l; };

test("frame codec round trip and resync", () => {
  const got: number[] = [], other: number[] = [];
  const d = new Decoder((f) => got.push(f.type), (b) => other.push(...b));
  const f = encodeFrame(1, 2, Uint8Array.of(1, 2, 3));
  const bad = encodeFrame(1, 9, Uint8Array.of(7)); bad[5] ^= 0xff;
  d.push(Uint8Array.from([0x68, 0x69, ...bad, ...f]));
  expect(got).toEqual([2]);
  expect(other.slice(0, 2)).toEqual([0x68, 0x69]);
  expect(crc16(new TextEncoder().encode("123456789"))).toBe(0x29b1);
});

test("hello and ping", async () => {
  const l = new Link(await connectTcp("127.0.0.1", PORT));
  const info = await l.hello();
  expect(info.os).toBe("PalanuOS");
  expect(await l.ping()).toBeLessThan(500);
  l.close();
});

test("files: write, read back, list, large and binary", async () => {
  const l = await open();
  const data = new Uint8Array(5000).map((_, i) => (i * 7 + 3) & 255);
  await l.write("/home/link-test.bin", data);
  expect(Buffer.from(await l.read("/home/link-test.bin")).equals(Buffer.from(data))).toBe(true);
  expect((await l.list("/home")).some((e) => e.name === "link-test.bin" && e.size === 5000)).toBe(true);
  await expect(l.read("/home/does-not-exist")).rejects.toThrow();
  l.close();
});

test("files: make a folder, rename, remove (and a missing path is an error)", async () => {
  const l = await open();
  await l.mkdir("/home/linkdir");
  await l.write("/home/linkdir/a.txt", new TextEncoder().encode("hi"));
  await l.rename("/home/linkdir/a.txt", "/home/linkdir/b.txt");
  const names = (await l.list("/home/linkdir")).map((e) => e.name);
  expect(names).toEqual(["b.txt"]);
  await l.remove("/home/linkdir/b.txt");
  await l.remove("/home/linkdir");
  expect((await l.list("/home")).some((e) => e.name === "linkdir")).toBe(false);
  await expect(l.remove("/home/no-such-file")).rejects.toThrow();
  l.close();
});

test("remote shell runs commands", async () => {
  const l = await open();
  let out = "";
  l.onTty = (d) => (out += new TextDecoder().decode(d));
  l.openShell(80, 24);
  await Bun.sleep(400);
  l.ttyWrite(new TextEncoder().encode("echo link-42\n"));
  for (let i = 0; i < 40 && !out.includes("link-42"); i++) await Bun.sleep(100);
  expect(out).toMatch(/\nlink-42\r\n/);
  l.closeShell();
  l.close();
}, 30000);

test("screen mirror delivers a full frame", async () => {
  const l = await open();
  l.subscribeScreen(true);
  for (let i = 0; i < 50 && l.screen.updates < 1; i++) await Bun.sleep(100);
  expect(l.screen.w).toBe(320);
  expect(l.screen.h).toBe(240);
  expect(l.screen.updates).toBeGreaterThan(0);
  expect(new Set(l.screen.px).size).toBe(2); // ink and paper: the 1-bit screens, not a blank one
  expect(l.screenPng().length).toBeGreaterThan(200);
  l.close();
});

test("many connections in a row do not crash the device", async () => {
  for (let i = 0; i < 15; i++) { const l = await open(); l.close(); }
  const l = await open();
  expect(await l.ping()).toBeLessThan(500);
  l.close();
});

test("remote key opens the first app and the mirror follows", async () => {
  const l = await open();
  l.subscribeScreen(true);
  for (let i = 0; i < 50 && l.screen.updates < 1; i++) await Bun.sleep(100);
  const before = l.screen.updates;
  l.key(2); // PAL_KEY_ENTER
  for (let i = 0; i < 50 && l.screen.updates === before; i++) await Bun.sleep(100);
  expect(l.screen.updates).toBeGreaterThan(before);
  l.key(7); // PAL_KEY_ESC would be 3; 7 is DELETE: harmless
  l.close();
});

test("a package written to /apps becomes a command and runs as a process", async () => {
  const dir = join(import.meta.dir, "../../../../sdk/examples");
  const work = mkdtempSync(join(tmpdir(), "papp-"));
  const build = async (src: string, id: string, extra: string[] = []) => {
    const out = join(work, id + ".papp");
    const r = Bun.spawnSync(["bun", join(import.meta.dir, "../../../tools/palanu.ts"), "build", join(dir, src), "--id", id, "-o", out, ...extra]);
    if (r.exitCode !== 0) throw new Error(new TextDecoder().decode(r.stderr));
    return new Uint8Array(await Bun.file(out).arrayBuffer());
  };
  const l = await open();
  await l.write("/apps/hello.papp", await build("hello.c", "hello"));
  await l.write("/apps/count.papp", await build("count.c", "count"));
  await l.write("/apps/spin.papp", await build("spin.c", "spin", ["--fuel", "20"]));
  let out = "";
  l.onTty = (d) => (out += new TextDecoder().decode(d));
  l.openShell(80, 24);
  await Bun.sleep(400);
  const run = async (cmd: string, want: RegExp) => {
    out = "";
    l.ttyWrite(new TextEncoder().encode(cmd + "\n"));
    for (let i = 0; i < 60 && !want.test(out); i++) await Bun.sleep(100);
    expect(out).toMatch(want);
  };
  await run("hello one two", /arg: two/);
  await run("echo a b c | count", /\n1 3 6\r\n/);
  await run("spin", /instruction/i); // the watchdog stops a runaway app
  await run("app list", /hello/);
  l.closeShell();
  l.close();
}, 30000);
