// End-to-end test of the SSH app (sdk/examples/ssh, library sdk/palssh). The app is built with `palanu build`, installed
// in the host simulator, and run there from the simulator's shell. The server is a throwaway OpenSSH sshd on localhost
// (non-root, high port, pubkey auth, StrictModes no). Nothing here is part of the device build.
//   bun forge/tests/ssh_app_e2e.ts
// Needs: build/host/palanu-sim (cmake --build build/host --target palanu-sim), /usr/sbin/sshd, ssh-keygen.
import { appendFileSync, copyFileSync, mkdirSync, mkdtempSync, openSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir, userInfo } from "node:os";
import { join } from "node:path";
import { Link, connectTcp } from "../packages/link/src/index.ts";
import { pappId } from "../tools/palanu_app.ts";

const ROOT = join(import.meta.dir, "../..");
const SIM = join(ROOT, "build/host/palanu-sim");
const APP_DIR = join(ROOT, "sdk/examples/ssh");
const SSHD_PORT = 2222;
const LINK_PORT = Number(process.env.SSH_E2E_LINK_PORT ?? 7100);
const user = userInfo().username;
const dec = new TextDecoder(), enc = new TextEncoder();
const sleep = (ms: number) => Bun.sleep(ms);

function run(cmd: string[], cwd = ROOT): string {
  const r = Bun.spawnSync(cmd, { cwd, stdout: "pipe", stderr: "pipe" });
  if (r.exitCode !== 0) throw new Error(`${cmd.join(" ")} failed:\n${r.stderr.toString()}${r.stdout.toString()}`);
  return r.stdout.toString();
}

// ---- the terminal of the simulator's shell ----
class Term {
  private text = "";
  constructor(private link: Link) {
    link.onTty = (d) => { this.text += dec.decode(d, { stream: true }); };
  }
  clean(): string {
    return this.text.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "").replace(/\r/g, "");
  }
  size(): number { return this.clean().length; } // the same text expect() slices
  type(s: string) { this.link.ttyWrite(enc.encode(s)); }
  raw(bytes: number[]) { this.link.ttyWrite(Uint8Array.from(bytes)); }
  // Waits until re matches the output after `from`. Returns the match or null on timeout.
  async expect(re: RegExp, ms = 15000, from = 0): Promise<RegExpExecArray | null> {
    const t = Date.now();
    for (;;) {
      const m = re.exec(this.clean().slice(from));
      if (m) return m;
      if (Date.now() - t > ms) return null;
      await sleep(100);
    }
  }
  tail(from: number): string { return this.clean().slice(from); }
}

// ---- the server ----
function startSshd(dir: string): { proc: ReturnType<typeof Bun.spawn>; hostPub: string } {
  const hostKey = join(dir, "host");
  run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", hostKey]);
  writeFileSync(join(dir, "authorized_keys"), "");
  writeFileSync(join(dir, "sshd_config"), [
    `Port ${SSHD_PORT}`, "ListenAddress 127.0.0.1", `HostKey ${hostKey}`, `PidFile ${join(dir, "pid")}`,
    `AuthorizedKeysFile ${join(dir, "authorized_keys")}`, "PasswordAuthentication no",
    "KbdInteractiveAuthentication no", "PubkeyAuthentication yes", "UsePAM no", "StrictModes no", "LogLevel ERROR",
  ].join("\n") + "\n");
  const log = openSync(join(dir, "sshd.log"), "w");
  const proc = Bun.spawn(["/usr/sbin/sshd", "-D", "-e", "-f", join(dir, "sshd_config")], { stdout: "ignore", stderr: log });
  return { proc, hostPub: readFileSync(`${hostKey}.pub`, "utf8") };
}

// A second sshd on another port that offers only one cipher: the app's list prefers aes128-gcm, so this forces the others.
function startSshdCipher(dir: string, port: number, cipher: string, hostKey: string): ReturnType<typeof Bun.spawn> {
  const cfg = join(dir, `sshd_${port}_config`);
  writeFileSync(cfg, [
    `Port ${port}`, "ListenAddress 127.0.0.1", `HostKey ${hostKey}`, `PidFile ${join(dir, `pid_${port}`)}`,
    `AuthorizedKeysFile ${join(dir, "authorized_keys")}`, "PasswordAuthentication no",
    "KbdInteractiveAuthentication no", "PubkeyAuthentication yes", "UsePAM no", "StrictModes no", "LogLevel DEBUG1",
    `Ciphers ${cipher}`,
  ].join("\n") + "\n");
  return Bun.spawn(["/usr/sbin/sshd", "-D", "-e", "-f", cfg], { stdout: "ignore", stderr: openSync(join(dir, `sshd_${port}.log`), "w") });
}

async function waitPort(port: number, ms = 10000): Promise<boolean> {
  const t = Date.now();
  while (Date.now() - t < ms) {
    try {
      const s = await Bun.connect({ hostname: "127.0.0.1", port, socket: { data() {}, open(sock) { sock.end(); }, close() {}, error() {} } });
      s.end();
      return true;
    } catch { await sleep(200); }
  }
  return false;
}

// ---- main ----
const transcript: string[] = [];
let failed = 0;
const check = (name: string, ok: boolean, detail = "") => {
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${!ok && detail ? `\n     ${detail.slice(-600).replace(/\n/g, "\n     ")}` : ""}`);
  if (!ok) failed++;
};

// The built-in `ssh` (apps/cmd_ssh.c, the libssh client) shadows /apps/ssh.papp in the shell until it is removed (T4.5).
// So the test installs the same sources under the id `sshapp`, built from a temporary copy of the app directory.
const dir = mkdtempSync(join(tmpdir(), "palanu-sshapp-"));
const build = run([process.execPath, join(ROOT, "forge/tools/palanu.ts"), "build"], APP_DIR);
const papp = join(APP_DIR, "ssh.papp");
console.log(`built sdk/examples/ssh: ${statSync(papp).size} bytes (${build.trim()})`);
const testApp = join(dir, "app");
mkdirSync(testApp);
copyFileSync(join(APP_DIR, "main.c"), join(testApp, "main.c"));
writeFileSync(join(testApp, "app.toml"), readFileSync(join(APP_DIR, "app.toml"), "utf8").replace(/^id = "ssh"$/m, 'id = "sshapp"'));
run([process.execPath, join(ROOT, "forge/tools/palanu.ts"), "build"], testApp);
const appBytes = new Uint8Array(readFileSync(join(testApp, "sshapp.papp")));
const appId = pappId(appBytes);
const CMD = appId;
// The app sees /data as its own directory: /apps/<id>/data in the system (api/fs.c).
const KNOWN_HOSTS = `/apps/${appId}/data/known_hosts`;
const knownHostsKeyLine = (pub: string, host = `[127.0.0.1]:${SSHD_PORT}`) => `${host} ${pub.trim().split(" ").slice(0, 2).join(" ")}\n`;
const sshd = startSshd(dir);
if (!(await waitPort(SSHD_PORT))) throw new Error("sshd did not start");
// --link-open trusts the TCP link without pairing: tests only (docs/PLP.md), as forge/tools/hwtest.ts does.
const sim = Bun.spawn([SIM, "--link", String(LINK_PORT), "--link-open", "--wait", "600000"], {
  env: { ...process.env, PALANU_DATA: join(dir, "data") }, stdout: "ignore", stderr: "ignore",
});
let link: Link | undefined;
for (let i = 0; i < 100 && !link; i++) {
  try { link = new Link(await connectTcp("127.0.0.1", LINK_PORT)); } catch { await sleep(200); }
}
if (!link) throw new Error("the simulator did not start");
await link.hello();
const extra: ReturnType<typeof Bun.spawn>[] = [];
const cleanup = () => { try { sim.kill(); } catch {} try { sshd.proc.kill(); } catch {} for (const p of extra) try { p.kill(); } catch {} };

try {
  await link.write(`/apps/${appId}.papp`, appBytes);
  const term = new Term(link);
  link.openShell(100, 30);
  await sleep(1000);
  link.resize(100, 30); // the app reads the terminal size when it starts: make it the size the test expects
  await sleep(500);
  term.type("\n");
  await sleep(300);

  // keys in the keystore (ssh keygen / ssh pub) and the authorized_keys line for sshd
  term.type(`${CMD} keygen\n`);
  const kg = await term.expect(/made key id_ed25519/, 15000);
  check("ssh keygen makes a key in the keystore", !!kg, term.tail(0));
  let mark = term.size();
  term.type(`${CMD} pub\n`);
  const pubM = await term.expect(/(ssh-ed25519 [A-Za-z0-9+/=]+ id_ed25519)/, 15000, mark);
  check("ssh pub prints the authorized_keys line", !!pubM, term.tail(mark));
  if (!pubM) throw new Error("no public key line");
  appendFileSync(join(dir, "authorized_keys"), pubM[1] + "\n");
  transcript.push(`$ ${CMD} pub\n${pubM[1]}`);

  // exec with a host key the device has never seen: asked, then the command runs and the status comes back
  mark = term.size();
  const t0 = Date.now();
  term.type(`${CMD} -p ${SSHD_PORT} ${user}@127.0.0.1 'echo hello; uname'; echo __RC_$?_RC\n`);
  const ask = await term.expect(/\(yes\/no\)\? /, 20000, mark);
  check("unknown host key is shown with its SHA256 fingerprint and asked", !!ask && /SHA256:[A-Za-z0-9+/]{43}/.test(term.tail(mark)), term.tail(mark));
  term.type("yes\n");
  const tyes = Date.now();
  const ran = await term.expect(/hello\n[^\n]*Darwin/, 20000, mark);
  check("exec command runs on the server (echo hello; uname)", !!ran, term.tail(mark));
  check("exec returns the remote status (0)", !!(await term.expect(/__RC_0_RC/, 10000, mark)), term.tail(mark));
  transcript.push(`$ ${CMD} -p ${SSHD_PORT} ${user}@127.0.0.1 'echo hello; uname'\n${term.tail(mark).trim()}`);
  const kh = await link.read(KNOWN_HOSTS).then((b) => dec.decode(b), () => "");
  check("the accepted host is stored in known_hosts", kh.includes(`[127.0.0.1]:${SSHD_PORT} ssh-ed25519`), kh);
  console.log(`     exec: connect, key exchange, auth and run took ${Date.now() - tyes} ms after the host key was accepted (${Date.now() - t0} ms in all)`);

  // interactive shell with a PTY: the remote sees the size, and a resize reaches it
  mark = term.size();
  term.type(`${CMD} -p ${SSHD_PORT} ${user}@127.0.0.1; echo __RC_$?_RC\n`);
  // the remote shell starts a moment after the session: retry the query until it answers
  let pty = null;
  for (let i = 0; i < 8 && !pty; i++) {
    await sleep(1500);
    term.type("stty size\n");
    pty = await term.expect(/\b30 100\b/, 1000, mark);
  }
  check("interactive shell has a PTY of the terminal size (30 rows, 100 columns)", !!pty, "size lines: " + JSON.stringify(term.tail(mark).match(/\d+ \d+/g)));
  link.resize(120, 40);
  await sleep(800);
  const m2 = term.size();
  term.type("stty size\n");
  check("window change reaches the server (40 rows, 120 columns)", !!(await term.expect(/\b40 120\b/, 10000, m2)), term.tail(m2));
  transcript.push(`$ stty size   (after resize to 120x40)\n${term.tail(m2).trim()}`);

  // Ctrl-C goes to the server as byte 0x03: it stops `sleep 30` and the next command runs
  const m3 = term.size();
  term.type("sleep 30\n");
  await sleep(1500);
  const tc = Date.now();
  term.raw([0x03]);
  await sleep(400);
  term.type("echo ctrlc-$((2*21))\n");
  const after = await term.expect(/ctrlc-42/, 10000, m3);
  check("Ctrl-C reaches the server (sleep 30 is interrupted)", !!after && Date.now() - tc < 10000, term.tail(m3));
  transcript.push(`$ sleep 30   ^C\n${term.tail(m3).trim()}`);

  // exit status: the remote `exit 7` ends the session and ssh returns 7 to the local shell
  term.type("exit 7\n");
  check("remote exit closes the session and returns its status (7)", !!(await term.expect(/__RC_7_RC/, 10000, m3)), term.tail(m3));

  // keys: list, import an OpenSSH ed25519 key file from the app's /data, print it back
  mark = term.size();
  term.type(`${CMD} keys\n`);
  check("ssh keys lists the keystore", !!(await term.expect(/id_ed25519/, 10000, mark)), term.tail(mark));
  run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", join(dir, "imp")]);
  await link.write(`/apps/${appId}/data/imp_key`, new Uint8Array(readFileSync(join(dir, "imp"))));
  mark = term.size();
  term.type(`${CMD} import imported /data/imp_key\n`);
  check("ssh import brings an OpenSSH ed25519 key into the keystore", !!(await term.expect(/imported key imported/, 10000, mark)), term.tail(mark));
  mark = term.size();
  term.type(`${CMD} pub imported\n`);
  const impPub = readFileSync(join(dir, "imp.pub"), "utf8").trim().split(" ")[1];
  const impM = await term.expect(/ssh-ed25519 ([A-Za-z0-9+/=]+) imported/, 10000, mark);
  check("the imported key has the public key of the file", !!impM && impM[1] === impPub, term.tail(mark));

  // without a usable key the server refuses: "Permission denied", status 255
  mark = term.size();
  term.type(`${CMD} -i nosuchkey -p ${SSHD_PORT} ${user}@127.0.0.1 true; echo __RC_$?_RC\n`);
  check("no usable key: permission denied with status 255", !!(await term.expect(/Permission denied[\s\S]*__RC_255_RC/, 15000, mark)), term.tail(mark));

  // `~.` at the start of a line ends the session from the client side (the escape of ssh(1))
  mark = term.size();
  term.type(`${CMD} -p ${SSHD_PORT} ${user}@127.0.0.1; echo __RC_$?_RC\n`);
  let ready = null;
  for (let i = 0; i < 8 && !ready; i++) {
    await sleep(1500);
    term.type("echo ready-$((3*3))\n");
    ready = await term.expect(/ready-9/, 1000, mark);
  }
  check("a second session starts", !!ready, term.tail(mark));
  term.raw([0x0d]);
  await sleep(200);
  term.type("~.");
  check("~. at the start of a line disconnects with status 255", !!(await term.expect(/Connection to 127.0.0.1 closed\.[\s\S]*__RC_255_RC/, 10000, mark)), term.tail(mark));

  // the other two ciphers, against servers that offer only that one
  for (const [port, cipher] of [[2223, "aes256-gcm@openssh.com"], [2224, "chacha20-poly1305@openssh.com"]] as const) {
    extra.push(startSshdCipher(dir, port, cipher, join(dir, "host")));
    if (!(await waitPort(port))) throw new Error(`sshd on ${port} did not start`);
    const known = await link.read(KNOWN_HOSTS).then((b) => dec.decode(b), () => "");
    await link.write(KNOWN_HOSTS, enc.encode(known + `[127.0.0.1]:${port} ${sshd.hostPub.trim().split(" ").slice(0, 2).join(" ")}\n`));
    mark = term.size();
    term.type(`${CMD} -p ${port} ${user}@127.0.0.1 'echo cipher-ok'; echo __RC_$?_RC\n`);
    const okc = !!(await term.expect(/cipher-ok\n[\s\S]*__RC_0_RC/, 20000, mark));
    check(`exec over ${cipher} (the server offers only it)`, okc, okc ? "" : readFileSync(join(dir, `sshd_${port}.log`), "utf8").split("\n").slice(-12).join("\n"));
  }

  // a changed host key is refused, with no way to override it
  const other = join(dir, "other");
  run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", other]);
  await link.write(KNOWN_HOSTS, enc.encode(knownHostsKeyLine(readFileSync(`${other}.pub`, "utf8"))));
  const m4 = term.size();
  term.type(`${CMD} -p ${SSHD_PORT} ${user}@127.0.0.1 true; echo __RC_$?_RC\n`);
  check("a changed host key is refused with a warning", !!(await term.expect(/REMOTE HOST IDENTIFICATION HAS CHANGED/, 15000, m4)), term.tail(m4));
  check("the refused connection exits 255", !!(await term.expect(/__RC_255_RC/, 10000, m4)), term.tail(m4));
  transcript.push(`$ ${CMD} ... (host key changed)\n${term.tail(m4).trim()}`);

  console.log("\ntranscripts:\n" + transcript.map((t) => "--- \n" + t).join("\n"));
  console.log(`\nmeasured: ${appId}.papp ${appBytes.length} bytes (sdk/examples/ssh builds to ${statSync(papp).size} bytes)`);
  link.closeShell();
} finally {
  link.close();
  cleanup();
  rmSync(dir, { recursive: true, force: true });
}
console.log(failed ? `\n${failed} check(s) failed` : "\nall checks passed");
process.exit(failed ? 1 : 0);
