// Pairing and auth over TCP against a simulator started WITHOUT --link-open (docs/PLP.md, "Trust"). The first pairing code
// comes from the simulator's log (--log prints it when the scripted `link pair` runs); later codes are read from the shell
// of an authenticated connection. The TCP link serves one connection at a time, so each test closes its connection before
// the next one opens. Needs build/host/palanu-sim (./pal build host).
import { afterAll, beforeAll, expect, test } from "bun:test";
import { existsSync, mkdtempSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { authenticate, connectTcp, Link, LinkError } from "../src/index.ts";
import { LINK_ERR } from "../src/auth.ts";
import { forgetPairing, linksPath, loadPairing, savePairing } from "../src/tokens.ts";

const ROOT = join(import.meta.dir, "../../../..");
const SIM = join(ROOT, "build/host/palanu-sim");
const PALANU = join(ROOT, "forge/tools/palanu.ts");
const PORT = 7900 + Math.floor(Math.random() * 90);
const ADDR = `127.0.0.1:${PORT}`;
const enc = new TextEncoder(), dec = new TextDecoder();
const CONFIG = mkdtempSync(join(tmpdir(), "palanu-cfg-"));
const EMPTY = mkdtempSync(join(tmpdir(), "palanu-empty-"));
process.env.PALANU_CONFIG_DIR = CONFIG; // the token store the tests write and read (tokens.ts reads it on each call)

const CODE = /pairing code:? (\d{6})/;
let sim: ReturnType<typeof Bun.spawn> | undefined;
let firstCode = "";
let deviceId = "";
let secondCode = "";
let tablet: { clientId: string; token: string } | undefined;

beforeAll(async () => {
  if (!existsSync(SIM)) throw new Error("build the simulator first: ./pal build host");
  const data = mkdtempSync(join(tmpdir(), "palanu-auth-"));
  // scripted: open the terminal from the launcher (Enter), run `link pair` there, and print the code to the log (stdout
  // here). Then the device lives on for the tests.
  sim = Bun.spawn([SIM, "--log", "--link", String(PORT), "--wait", "2000", "--key", "enter", "--wait", "400", "--key", "enter", "--wait", "800", "--cmd", "link pair", "--wait", "200000"], {
    env: { ...process.env, PALANU_DATA: data }, stdout: "pipe", stderr: "ignore",
  });
  const reader = (sim.stdout as ReadableStream<Uint8Array>).getReader();
  let text = "";
  const deadline = Date.now() + 20000;
  while (!firstCode && Date.now() < deadline) {
    const { value, done } = await reader.read();
    if (done) break;
    text += dec.decode(value);
    const m = CODE.exec(text);
    if (m) firstCode = m[1];
  }
  if (!firstCode) throw new Error("the simulator did not print a pairing code");
  (async () => { while (!(await reader.read()).done) { /* keep the pipe drained: the device logs for as long as it lives */ } })();
  for (let i = 0; i < 50; i++) { try { (await connectTcp("127.0.0.1", PORT)).close(); return; } catch { await Bun.sleep(100); } }
  throw new Error("simulator did not start");
}, 40000);
afterAll(() => { sim?.kill(); });

const open = async () => { const l = new Link(await connectTcp("127.0.0.1", PORT)); const info = await l.hello(); return { l, info }; };

/** Types a line on an open shell and waits until the output matches `want`. */
async function shell(l: Link, line: string, want: RegExp, ms = 5000): Promise<string> {
  let out = "";
  const prev = l.onTty;
  l.onTty = (d) => { out += dec.decode(d); };
  l.ttyWrite(enc.encode(line + "\n"));
  for (let i = 0; i < ms / 100 && !want.test(out); i++) await Bun.sleep(100);
  l.onTty = prev;
  return out;
}

async function openShell(l: Link) {
  l.openShell(80, 24);
  await Bun.sleep(400);
}

const errCode = (f: { payload: Uint8Array }) => (f.payload[0] << 24) >> 24;

test("an untrusted connection answers hello and ping, and nothing else", async () => {
  const { l, info } = await open();
  deviceId = String(info.id);
  expect(info.trusted).toBe(false);
  expect(info.chip).toBeUndefined(); // basic INFO only
  expect(deviceId).toMatch(/^[0-9a-f]{16}$/);
  expect(await l.ping()).toBeLessThan(500);
  await expect(l.list("/")).rejects.toThrow("device error -23");
  await expect(l.write("/home/refused.bin", new Uint8Array(4))).rejects.toThrow("device error -23");
  const tty = l.wait((f) => f.chan === 1 && f.type === 0x7f, 3000);
  l.openShell(80, 24);
  expect(errCode(await tty)).toBe(-23);
  const scr = l.wait((f) => f.chan === 2 && f.type === 0x7f, 3000);
  l.subscribeScreen(true);
  expect(errCode(await scr)).toBe(-23);
  l.close();
});

test("a wrong code is counted; the right one pairs and the token is stored with mode 600", async () => {
  const { l } = await open();
  const session = await l.pairStart("test-laptop");
  const wrong = firstCode === "000000" ? "111111" : "000000";
  const err = (await session.prove(wrong).catch((e) => e)) as LinkError;
  expect(err).toBeInstanceOf(LinkError);
  expect(err.code).toBe(LINK_ERR.auth);
  expect(err.message).toContain("2 tries left");
  const paired = await session.prove(firstCode);
  expect(paired.clientId).toMatch(/^[0-9a-f]{16}$/);
  expect(paired.token).toMatch(/^[0-9a-f]{64}$/);
  savePairing(deviceId, { clientId: paired.clientId, token: paired.token, name: "test-laptop", paired: "now" });
  expect(statSync(linksPath()).mode & 0o777).toBe(0o600);
  l.close();
});

test("a stored token authenticates: full INFO and a shell; the device shows the next code from that shell", async () => {
  const { l, info } = await open();
  const full = await authenticate(l, info, `palanu pair --tcp ${ADDR}`);
  expect(full.trusted).toBe(true);
  expect(full.chip).toBeDefined();
  await openShell(l);
  expect(await shell(l, "echo auth-ok", /auth-ok/)).toMatch(/auth-ok/);
  const m = CODE.exec(await shell(l, "link pair", CODE));
  expect(m).not.toBeNull(); // the window of the first code closed on success; this opens another
  secondCode = m![1];
  l.close();
});

test("a wrong token is refused", async () => {
  const p = loadPairing(deviceId)!;
  const { l } = await open();
  const bad = p.token.slice(0, -2) + (p.token.endsWith("00") ? "11" : "00");
  const err = (await l.auth(p.clientId, bad).catch((e) => e)) as LinkError;
  expect(err.code).toBe(LINK_ERR.auth);
  l.close();
});

test("a second client pairs with the second code, and three wrong codes close the window", async () => {
  const { l: tabl } = await open();
  const session = await tabl.pairStart("tablet");
  // two wrong codes (the device counts them), then the right one
  const w1 = secondCode === "000001" ? "000002" : "000001";
  const w2 = secondCode === "000003" ? "000004" : "000003";
  expect(((await session.prove(w1).catch((e) => e)) as LinkError).code).toBe(LINK_ERR.auth);
  expect(((await session.prove(w2).catch((e) => e)) as LinkError).code).toBe(LINK_ERR.auth);
  tablet = await session.prove(secondCode);
  expect(tablet.clientId).not.toBe(loadPairing(deviceId)!.clientId);
  tabl.close();

  // a fresh window from the laptop's shell: three wrong codes, and the window is closed for the right one too
  const { l: laptop, info } = await open();
  await authenticate(laptop, info, `palanu pair --tcp ${ADDR}`);
  await openShell(laptop);
  const code3 = CODE.exec(await shell(laptop, "link pair", CODE))![1];
  laptop.close();
  const { l: c } = await open();
  const s = await c.pairStart("late");
  const wrong = code3 === "000007" ? "000008" : "000007";
  for (let i = 0; i < 3; i++) {
    const e = (await s.prove(wrong).catch((x) => x)) as LinkError;
    expect(e.code).toBe(i < 2 ? LINK_ERR.auth : LINK_ERR.pairClosed);
  }
  expect(((await s.prove(code3).catch((x) => x)) as LinkError).code).toBe(LINK_ERR.pairClosed);
  c.close();
});

test("a client revokes itself from its shell: its live session ends and its token stops working", async () => {
  // the tablet connects, opens a shell, and removes its own client record; the device drops the connection after that
  const { l: tab } = await open();
  const t = tablet!;
  await tab.auth(t.clientId, t.token); // the tablet's own id and token (authenticate() would pick the laptop's)
  await openShell(tab);
  expect(await shell(tab, `link revoke ${t.clientId}`, /revoked/)).toContain("revoked " + t.clientId);
  // the device checks the session between reads (about every 0.5 s) and closes the socket: a ping then gets no answer
  await Bun.sleep(1200);
  const dropped = await tab.ping().then(() => false, () => true);
  tab.close();
  expect(dropped).toBe(true);
  const { l: again } = await open();
  const err = (await again.auth(t.clientId, t.token).catch((e) => e)) as LinkError;
  expect(err.code).toBe(LINK_ERR.auth);
  again.close();
});

test("the CLI refuses an unpaired device, and pairs with a code read from its input", async () => {
  const bad = Bun.spawnSync(["bun", PALANU, "info", "--tcp", ADDR], { env: { ...process.env, PALANU_CONFIG_DIR: EMPTY } });
  expect(bad.exitCode).toBe(1);
  expect(new TextDecoder().decode(bad.stderr)).toContain(`not paired: run \`palanu pair --tcp ${ADDR}\``);

  // a code for the CLI comes from the shell of the laptop (its stored token), then that connection closes
  const { l, info } = await open();
  await authenticate(l, info, `palanu pair --tcp ${ADDR}`);
  await openShell(l);
  const code = CODE.exec(await shell(l, "link pair", CODE))![1];
  l.close();

  const wrong = code === "000005" ? "000006" : "000005";
  const cli = Bun.spawn(["bun", PALANU, "pair", "--tcp", ADDR, "--name", "cli-test"], {
    env: { ...process.env, PALANU_CONFIG_DIR: CONFIG }, stdin: "pipe", stdout: "pipe", stderr: "pipe",
  });
  cli.stdin.write(`${wrong}\n${code}\n`);
  cli.stdin.end();
  const exit = await cli.exited;
  const err = await new Response(cli.stderr).text();
  expect(exit).toBe(0);
  expect(err).toContain("wrong code");
  expect(loadPairing(deviceId)?.name).toBe("cli-test");
  expect(statSync(linksPath()).mode & 0o777).toBe(0o600);

  // with the token stored, an ordinary command authenticates by itself
  const ok = Bun.spawnSync(["bun", PALANU, "info", "--tcp", ADDR], { env: { ...process.env, PALANU_CONFIG_DIR: CONFIG } });
  expect(ok.exitCode).toBe(0);
  expect(new TextDecoder().decode(ok.stdout)).toContain('"trusted": true');
  forgetPairing(deviceId);
  expect(loadPairing(deviceId)).toBeUndefined();
}, 60000);
