// End-to-end test of scp and sftp (apps/cmd_sftp.c, ADR-0017): a simulator built with libssh (docs/TESTING-SSH.md) copies files to
// and from a throwaway OpenSSH server on localhost (internal-sftp), authenticating with a key the simulator generated itself.
//   bun forge/tests/sftp_e2e.ts [path/to/palanu-sim]      default build/host-ssh/palanu-sim
import { appendFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Link, connectTcp } from "../packages/link/src/index.ts";

const SIM = process.argv[2] ?? join(import.meta.dir, "../../build/host-ssh/palanu-sim");
if (!existsSync(SIM)) throw new Error(`no simulator with libssh at ${SIM} (docs/TESTING-SSH.md)`);
const dir = mkdtempSync(join(tmpdir(), "palanu-sftpd-"));
const files = join(dir, "files");
mkdirSync(files);
const run = (cmd: string[]) => { const r = Bun.spawnSync(cmd); if (r.exitCode !== 0) throw new Error(cmd.join(" ") + ": " + r.stderr); };
run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", join(dir, "host")]);
writeFileSync(join(dir, "authorized_keys"), "");
writeFileSync(join(dir, "sshd_config"), [
  "Port 2223", "ListenAddress 127.0.0.1", `HostKey ${join(dir, "host")}`, `PidFile ${join(dir, "pid")}`,
  `AuthorizedKeysFile ${join(dir, "authorized_keys")}`, "PasswordAuthentication no", "KbdInteractiveAuthentication no",
  "PubkeyAuthentication yes", "UsePAM no", "StrictModes no", "LogLevel ERROR", "Subsystem sftp internal-sftp"].join("\n") + "\n");
const user = Bun.spawnSync(["whoami"]).stdout.toString().trim();
writeFileSync(join(files, "remote.txt"), "from the server\n");
const big = new Uint8Array(200_000).map((_, i) => (i * 13 + 7) & 255);
writeFileSync(join(files, "big.bin"), big);

const sshd = Bun.spawn(["/usr/sbin/sshd", "-D", "-e", "-f", join(dir, "sshd_config")], { stdout: "ignore", stderr: "ignore" });
const sim = Bun.spawn([SIM, "--link", "7793", "--link-open", "--wait", "120000"], { env: { ...process.env, PALANU_DATA: join(dir, "data") }, stdout: "ignore", stderr: "ignore" });
let l: Link | undefined;
for (let i = 0; i < 60 && !l; i++) { try { l = new Link(await connectTcp("127.0.0.1", 7793)); } catch { await Bun.sleep(200); } }
if (!l) throw new Error("simulator did not start");
await l.hello();

let out = "";
l.onTty = (d) => (out += new TextDecoder().decode(d));
const clean = () => out.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "");
const send = (s: string) => l!.ttyWrite(new TextEncoder().encode(s));
const waitFor = async (re: RegExp, ms = 15000) => { const t = Date.now(); while (!re.test(clean()) && Date.now() - t < ms) await Bun.sleep(100); return re.test(clean()); };
let failed = 0;
const check = (name: string, ok: boolean) => { console.log(`${ok ? "ok  " : "FAIL"} ${name}`); if (!ok) failed++; };
const mark = () => { out = ""; };

l.openShell(100, 30);
await Bun.sleep(800);
send("ssh-keygen\n");
check("ssh-keygen makes a key pair", await waitFor(/authorized_keys/));
appendFileSync(join(dir, "authorized_keys"), new TextDecoder().decode(await l.read("/home/.ssh/id_ed25519.pub")));
await l.write("/home/up.txt", new TextEncoder().encode("from the device\n"));
await l.write("/home/big.bin", big);

mark();
send(`scp -P 2223 /home/up.txt ${user}@127.0.0.1:${files}/up.txt\n`);
check("an unknown host key is shown for confirmation", await waitFor(/\(yes\/no\)/, 20000));
send("yes\n");
check("scp uploads a file", await waitFor(/up\.txt: 16 bytes/, 20000) && readFileSync(join(files, "up.txt"), "utf8") === "from the device\n");

mark();
send(`scp -P 2223 ${user}@127.0.0.1:${files}/remote.txt /home/\n`);
await waitFor(/bytes in/, 20000);
check("scp downloads into a folder by the file's name", new TextDecoder().decode(await l.read("/home/remote.txt")) === "from the server\n");

mark();
send(`scp -P 2223 ${user}@127.0.0.1:${files}/big.bin /home/big-back.bin\n`);
await waitFor(/200000 bytes/, 30000);
check("200 KB come back byte for byte", Buffer.from(await l.read("/home/big-back.bin")).equals(Buffer.from(big)));

mark();
send(`scp -P 2223 ${user}@127.0.0.1:${files}/nothing.txt /home/\n`);
check("a missing remote file says so", await waitFor(/no such file/, 15000));

mark();
send(`sftp -P 2223 ${user}@127.0.0.1\n`);
check("sftp connects and prompts", await waitFor(/sftp> /, 20000));
mark();
send(`cd ${files}\n`);
await waitFor(/sftp> /);
mark();
send("mkdir made\n");
await waitFor(/sftp> /);
check("mkdir makes a folder", existsSync(join(files, "made")));
mark();
send("put /home/big.bin made/\n");
await waitFor(/200000 bytes/, 30000);
check("put writes into a folder", existsSync(join(files, "made/big.bin")) && readFileSync(join(files, "made/big.bin")).equals(Buffer.from(big)));
mark();
send("ls made\n");
check("ls lists the folder", await waitFor(/big\.bin/));
mark();
send("rename made/big.bin made/moved.bin\n");
await waitFor(/sftp> /);
check("rename moves a file", existsSync(join(files, "made/moved.bin")) && !existsSync(join(files, "made/big.bin")));
mark();
send("get made/moved.bin /home/moved.bin\n");
await waitFor(/bytes in/, 30000);
check("get reads a file back", Buffer.from(await l.read("/home/moved.bin")).equals(Buffer.from(big)));
mark();
send("rm made/moved.bin\n");
await waitFor(/sftp> /);
mark();
send("rmdir made\n");
await waitFor(/sftp> /);
const gone = !existsSync(join(files, "made"));
check("rm and rmdir remove them", gone);
if (!gone) console.log(clean());
mark();
send("exit\n");
check("exit returns to the shell", await waitFor(/palanu:/, 8000));

sshd.kill();
sim.kill();
process.exit(failed ? 1 : 0);
