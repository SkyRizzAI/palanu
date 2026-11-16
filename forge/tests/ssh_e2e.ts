// End-to-end test of the SSH client: the simulator (built with libssh, see docs/TESTING-SSH.md) runs `ssh` against a
// throwaway OpenSSH server on localhost, authenticating with a key the simulator generated itself.
//   bun forge/tests/ssh_e2e.ts
import { appendFileSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Link, connectTcp } from "../packages/link/src/index.ts";

const SIM = join(import.meta.dir, "../../build/host/palanu-sim");
const dir = mkdtempSync(join(tmpdir(), "palanu-sshd-"));
const run = (cmd: string[]) => { const r = Bun.spawnSync(cmd); if (r.exitCode !== 0) throw new Error(cmd.join(" ") + ": " + r.stderr); };
run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", join(dir, "host")]);
writeFileSync(join(dir, "authorized_keys"), "");
writeFileSync(join(dir, "sshd_config"), [
  "Port 2222", "ListenAddress 127.0.0.1", `HostKey ${join(dir, "host")}`, `PidFile ${join(dir, "pid")}`,
  `AuthorizedKeysFile ${join(dir, "authorized_keys")}`, "PasswordAuthentication no", "KbdInteractiveAuthentication no",
  "PubkeyAuthentication yes", "UsePAM no", "StrictModes no", "LogLevel ERROR"].join("\n") + "\n");
const user = Bun.spawnSync(["whoami"]).stdout.toString().trim();

const sshd = Bun.spawn(["/usr/sbin/sshd", "-D", "-e", "-f", join(dir, "sshd_config")], { stdout: "ignore", stderr: "ignore" });
const sim = Bun.spawn([SIM, "--link", "7792", "--link-open", "--wait", "90000"], { env: { ...process.env, PALANU_DATA: join(dir, "data") }, stdout: "ignore", stderr: "ignore" });
let l: Link | undefined;
for (let i = 0; i < 60 && !l; i++) { try { l = new Link(await connectTcp("127.0.0.1", 7792)); } catch { await Bun.sleep(200); } }
if (!l) throw new Error("simulator did not start");
await l.hello();

let out = "";
l.onTty = (d) => (out += new TextDecoder().decode(d));
const clean = () => out.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "");
const send = (s: string) => l!.ttyWrite(new TextEncoder().encode(s));
const waitFor = async (re: RegExp, ms = 15000) => { const t = Date.now(); while (!re.test(clean()) && Date.now() - t < ms) await Bun.sleep(100); return re.test(clean()); };
let failed = 0;
const check = (name: string, ok: boolean) => { console.log(`${ok ? "ok  " : "FAIL"} ${name}`); if (!ok) failed++; };

l.openShell(100, 30);
await Bun.sleep(800);
send("ssh-keygen\n");
check("ssh-keygen makes a key pair", await waitFor(/authorized_keys/));
appendFileSync(join(dir, "authorized_keys"), new TextDecoder().decode(await l.read("/home/.ssh/id_ed25519.pub")));
send(`ssh -p 2222 ${user}@127.0.0.1\n`);
check("unknown host key is shown for confirmation", await waitFor(/\(yes\/no\)/, 20000));
send("yes\n");
await Bun.sleep(4000);
let m = clean().length;
send("stty size\n");
await Bun.sleep(1500);
check("remote shell runs and sees the terminal size", /30 100/.test(clean().slice(m)));
l.resize(120, 40);
await Bun.sleep(800);
m = clean().length;
send("stty size\n");
await Bun.sleep(1500);
check("window resize reaches the remote", /40 120/.test(clean().slice(m)));
send("exit\n");
check("closing the remote session returns to the local shell", await waitFor(/Connection closed/, 8000));

sshd.kill();
sim.kill();
process.exit(failed ? 1 : 0);
