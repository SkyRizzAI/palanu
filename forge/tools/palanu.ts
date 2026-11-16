#!/usr/bin/env bun
// palanu: talk to a PalanuOS device over Palanu Link, and build apps.
//   palanu new NAME [--kind tty|gui|service]                 create an app project
//   palanu build [--id ID ...]                               in a project: app.toml -> NAME.papp (flags: old form)
//   palanu pair [--name N] [--tcp host:port | --serial dev]   pair with the device: type the code `link pair` shows
//   palanu [--tcp host:port | --serial /dev/cu.x[@baud]] <info|ping|ls|get|put|install|shell|screen|key|ota> ...
//   palanu run 'COMMAND LINE'                                 one shell command on the device; prints its output, exits with its status
//   palanu wifi set SSID [--pass-stdin]                       join a Wi-Fi network from the computer (the password: stdin, or PALANU_WIFI_PASS; never an argument)
//   palanu wifi scan | svc ARGS... | logs [COMPONENT]         the device's own commands (`wifi scan`, `svc list`, `log`) run remotely
//   palanu deploy FILE.papp [--run] [--args 'ARGS'] [--tcp host:port | --serial dev]   install (as /apps/<id>.papp); --run starts it and streams its output
import { basename, dirname, join, resolve } from "node:path";
import { Link, LinkError, authenticate, connectSerial, connectTcp } from "../packages/link/src/index.ts";
import { LINK_ERR } from "../packages/link/src/auth.ts";
import { linksPath, savePairing } from "../packages/link/src/tokens.ts";
import { createInterface } from "node:readline";
import { hostname } from "node:os";
import { packPapp, publicKeyHex, seedFromText, type Manifest } from "../packages/papp/src/index.ts";
import { randomBytes } from "node:crypto";
import { existsSync as exists, writeFileSync } from "node:fs";
import { buildProject, compileWasm, newProject, pappId, runOnShell } from "./palanu_app.ts";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";

// ---- offline commands: no device needed ----

function opt(args: string[], name: string, dflt?: string): string | undefined {
  const i = args.indexOf(name);
  if (i < 0) return dflt;
  const v = args[i + 1];
  args.splice(i, 2);
  return v;
}

async function offline(cmd: string, a: string[]): Promise<boolean> {
  if (cmd === "new") {
    const dir = newProject(a[0] ?? "", opt(a, "--kind", "tty")!, process.cwd());
    console.log(`created ${dir}/ (main.c, app.toml, README.md)\nnext: cd ${basename(dir)} && palanu build`);
    return true;
  }
  // project mode: `palanu build` in a directory with app.toml (no positional file, no --id)
  if (cmd === "build" && !a.includes("--id") && (a.length === 0 || a[0].startsWith("-"))) {
    const built = await buildProject(process.cwd(), opt(a, "-o"));
    console.log(`${built.path}: ${built.bytes} bytes`);
    return true;
  }
  if (cmd === "keygen") { // palanu keygen OUT.key: a new Ed25519 seed (hex, mode 600); prints the public key
    const out = a[0];
    if (!out) throw new Error("usage: palanu keygen OUT.key");
    if (exists(out)) throw new Error(`${out} exists; keys are never overwritten`);
    const seed = randomBytes(32);
    writeFileSync(out, seed.toString("hex") + "\n", { mode: 0o600 });
    console.log(`wrote ${out} (keep it secret); public key (add to /etc/trust/<name>.pub on the device):\n${publicKeyHex(seed)}`);
    return true;
  }
  if (cmd !== "build" && cmd !== "pack") return false;
  const id = opt(a, "--id");
  const num = (flag: string) => {
    const v = opt(a, flag);
    return v === undefined ? undefined : Number(v);
  };
  const m: Manifest = {
    id: id ?? "",
    name: opt(a, "--name", id),
    ver: opt(a, "--ver", "0.1"),
    mem: num("--mem"),
    stack: num("--stack"),
    fuel: num("--fuel"),
    perm: (opt(a, "--perm") ?? "").split(",").filter(Boolean) as Manifest["perm"],
  };
  const out = opt(a, "-o");
  const input = a[0];
  if (!input || !m.id) throw new Error(`usage: palanu ${cmd} ${cmd === "build" ? "app.c" : "app.wasm"} --id ID [--name N] [--ver V] [--perm fs,net] [--mem KB] [--stack KB] [--fuel M] [-o out.papp]`);
  let wasmPath = input;
  if (cmd === "build") {
    wasmPath = join(mkdtempSync(join(tmpdir(), "palanu-build-")), "app.wasm");
    compileWasm([input], wasmPath); // -I sdk: apps include SDK libraries by their path under sdk/ (palanu_app.ts)
  }
  const keyFile = opt(a, "--sign"); // a seed from `palanu keygen`: the package is signed (the device trusts keys in /etc/trust)
  const sign = keyFile ? seedFromText(new Uint8Array(await Bun.file(keyFile).arrayBuffer())) : undefined;
  const papp = packPapp(new Uint8Array(await Bun.file(wasmPath).arrayBuffer()), m, { sign });
  const dest = out ?? join(dirname(resolve(input)), `${m.id}.papp`);
  await Bun.write(dest, papp);
  console.log(`${dest}: ${papp.length} bytes `);
  return true;
}


/** Lines from stdin, queued: a line that arrives before it is asked for (a pipe) is kept, not dropped. */
function lineReader() {
  const queued: string[] = [];
  let waiting: { resolve: (s: string) => void; reject: (e: Error) => void } | undefined;
  const rl = createInterface({ input: process.stdin });
  rl.on("line", (l) => {
    if (waiting) { const w = waiting; waiting = undefined; w.resolve(l); } else queued.push(l);
  });
  rl.on("close", () => { if (waiting) { const w = waiting; waiting = undefined; w.reject(new Error("no code given")); } });
  return {
    next: () => (queued.length ? Promise.resolve(queued.shift()!) : new Promise<string>((resolve, reject) => { waiting = { resolve, reject }; })),
    close: () => rl.close(),
  };
}

const args = process.argv.slice(2);
let target = process.env.PALANU_TARGET ?? "";
while (args[0]?.startsWith("--")) {
  const f = args.shift()!;
  if (f === "--tcp") target = "tcp:" + args.shift();
  else if (f === "--serial") target = "serial:" + args.shift();
  else { console.error("unknown option " + f); process.exit(2); }
}
// --tcp and --serial also count after the command: palanu deploy app.papp --tcp host:port
for (const f of ["--tcp", "--serial"]) {
  const i = args.indexOf(f);
  if (i >= 0 && args[i + 1] !== undefined) {
    target = (f === "--tcp" ? "tcp:" : "serial:") + args[i + 1];
    args.splice(i, 2);
  }
}
const [cmd, ...rest] = args;
if (cmd && (await offline(cmd, rest).catch((e) => { console.error("error:", (e as Error).message); process.exit(1); }))) process.exit(0);
if (!cmd || !target) {
  console.error("offline: palanu new NAME [--kind tty|gui|service] | palanu build [app.c --id ID ...] | pack app.wasm --id ID\nusage: palanu --tcp host:port | --serial /dev/cu.xxx[@baud]  <pair [--name N]|info|ping|ls PATH|get PATH [OUT]|put FILE PATH|install FILE.papp|deploy FILE.papp [--run]|shell|screen OUT.png|key NAME|ota FILE>\n(or set PALANU_TARGET=tcp:host:port)");
  process.exit(2);
}

const [kind, addr] = [target.slice(0, target.indexOf(":")), target.slice(target.indexOf(":") + 1)];
const transport = kind === "tcp"
  ? await connectTcp(addr.slice(0, addr.lastIndexOf(":")), Number(addr.slice(addr.lastIndexOf(":") + 1)))
  : connectSerial(addr.split("@")[0], Number(addr.split("@")[1] ?? 115200));
const link = new Link(transport);
let info = await link.hello();

try {
  // Palanu Link trust (docs/PLP.md): a cable is trusted by default; a wireless link needs a token from `palanu pair`.
  if (cmd !== "pair" && info.trusted !== true) info = await authenticate(link, info, `palanu pair --${kind} ${addr}`);
  switch (cmd) {
    case "info": console.log(JSON.stringify(info, null, 2)); break;
    case "ping": for (let i = 0; i < 4; i++) console.log(`ping ${(await link.ping()).toFixed(1)} ms`); break;
    case "ls":
      for (const e of await link.list(rest[0] ?? "/")) console.log(`${e.dir ? "d" : "-"} ${String(e.size).padStart(8)}  ${e.name}${e.dir ? "/" : ""}`);
      break;
    case "get": {
      const d = await link.read(rest[0]);
      if (rest[1]) await Bun.write(rest[1], d); else process.stdout.write(d);
      break;
    }
    case "install": {
      const file = rest[0], name = basename(file);
      if (!name.endsWith(".papp")) throw new Error("install takes a .papp file");
      await link.write(`/apps/${name}`, new Uint8Array(await Bun.file(file).arrayBuffer()));
      console.log(`installed ${name}; run it by typing ${name.slice(0, -5)} in a shell`);
      break;
    }
    case "deploy": {
      // install to /apps/<id>.papp (the id is in the manifest); --run then starts it on a shell and streams its output
      const argsAt = rest.indexOf("--args"), argsValueAt = argsAt >= 0 ? argsAt + 1 : -1;
      const argsText = argsAt >= 0 ? rest[argsValueAt] ?? "" : "";
      const file = rest.find((x, i) => !x.startsWith("--") && i !== argsValueAt);
      if (!file || !file.endsWith(".papp")) throw new Error("usage: palanu deploy FILE.papp [--run] [--args 'ARGS'] [--tcp host:port | --serial dev]");
      const bytes = new Uint8Array(await Bun.file(file).arrayBuffer());
      const appId = pappId(bytes);
      await link.write(`/apps/${appId}.papp`, bytes);
      if (!rest.includes("--run")) {
        console.log(`installed ${appId}.papp; run it by typing ${appId} in a shell, or add --run`);
        break;
      }
      const stdin = process.stdin;
      if (stdin.isTTY) stdin.setRawMode(true);
      stdin.on("data", (d) => link.ttyWrite(new Uint8Array(d))); // keys reach the app (paltui menus need them)
      process.on("SIGINT", () => link.ttyWrite(Uint8Array.of(3))); // Ctrl-C ends the app (QUIT), as on the device
      process.stdout.on("resize", () => link.resize(process.stdout.columns, process.stdout.rows));
      const line = [appId, argsText].join(" ").trimEnd();
      const code = await runOnShell(link, line, (d) => process.stdout.write(d), process.stdout.columns ?? 80, process.stdout.rows ?? 24);
      if (stdin.isTTY) stdin.setRawMode(false);
      stdin.pause();
      if (code < 0) throw new Error("the shell closed before the app finished");
      console.error(`[${appId} exited with code ${code}]`);
      process.exitCode = code & 255;
      break;
    }
    case "run":
    case "wifi":
    case "svc":
    case "logs": {
      // The headless commands (docs/RELEASE.md): a command line on the device's shell, output streamed back.
      let line: string;
      if (cmd === "run") line = rest.join(" ");
      else if (cmd === "logs") line = ["log", ...rest].join(" ");
      else if (cmd === "svc") line = ["svc", ...rest].join(" ");
      else if (rest[0] === "set") {
        const ssid = rest[1];
        if (!ssid || ssid.startsWith("--")) throw new Error("usage: palanu wifi set SSID [--pass-stdin]   (or PALANU_WIFI_PASS in the environment)");
        let pass = process.env.PALANU_WIFI_PASS ?? "";
        if (rest.includes("--pass-stdin")) pass = (await new Response(Bun.stdin.stream()).text()).replace(/\r?\n$/, "");
        if (/[\s"'\\$`;|&<>]/.test(ssid + pass)) throw new Error("the SSID and the password may not contain spaces or shell characters on this path: type `wifi connect` on the device instead");
        line = ["wifi connect", ssid, pass].filter((x) => x).join(" ");
        console.error(`joining ${ssid}…`);
      } else line = ["wifi", ...rest].join(" ");
      if (!line.trim()) throw new Error(`usage: palanu ${cmd} ...`);
      // Collected, not streamed: the line editor echoes the typed command, and a script wants only the command's own output.
      const chunks: Uint8Array[] = [];
      const code = await runOnShell(link, line, (d) => chunks.push(d), 100, 24);
      if (code < 0) throw new Error("the shell closed before the command finished");
      let text = new TextDecoder().decode(Buffer.concat(chunks));
      const echo = text.indexOf("echo __PLN_$?_END"); /* the echo of what runOnShell appended to the line */
      if (echo >= 0) text = text.slice(text.indexOf("\n", echo) + 1);
      if (!process.stdout.isTTY) text = text.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "").replace(/\r/g, ""); /* a pipe gets plain text */
      try { process.stdout.write(text); } catch { /* the reader went away (| head) */ }
      process.exitCode = code & 255;
      break;
    }
    case "ota": {
      const image = new Uint8Array(await Bun.file(rest[0]).arrayBuffer());
      if (info.ota !== true) throw new Error("this device does not support firmware updates over the link");
      console.log(`running ${info.slot} (${info.build}); sending ${image.length} bytes`);
      await link.ota(image, (d, t) => process.stdout.write(`\r${Math.round((100 * d) / t)}%`));
      console.log("\nsent and verified; the device is restarting into the new image");
      break;
    }
    case "put": await link.write(rest[1], new Uint8Array(await Bun.file(rest[0]).arrayBuffer())); console.log("ok"); break;
    case "pair": {
      // the code is shown on the device by `link pair`; three wrong codes close that window, so the device is asked again
      const deviceId = String(info.id ?? "");
      if (!deviceId) throw new Error("this device does not report an id: update its firmware");
      const nameAt = rest.indexOf("--name");
      const name = nameAt >= 0 && rest[nameAt + 1] ? rest[nameAt + 1] : hostname();
      const session = await link.pairStart(name);
      const codes = lineReader();
      console.error(`pairing with ${String(info.name ?? "the device")}: type the 6-digit code that \`link pair\` shows on it`);
      let paired: { clientId: string; token: string } | undefined;
      try {
        for (let tries = 1; !paired; tries++) {
          process.stderr.write("code: ");
          const code = (await codes.next()).trim();
          try {
            paired = await session.prove(code);
          } catch (e) {
            if (!(e instanceof LinkError) || e.code !== LINK_ERR.auth || tries >= 3) throw e;
            console.error(`wrong code (${e.message}); try again`);
          }
        }
      } finally {
        codes.close();
      }
      savePairing(deviceId, { clientId: paired.clientId, token: paired.token, name, paired: new Date().toISOString() });
      console.log(`paired with ${String(info.name ?? "the device")}; the token is saved in ${linksPath()} (mode 600)`);
      break;
    }
    case "screen": {
      link.subscribeScreen(true);
      const t0 = Date.now();
      while ((link.screen.updates < 1 || Date.now() - t0 < 1500) && Date.now() - t0 < 15000) await Bun.sleep(100);
      await Bun.write(rest[0] ?? "screen.png", link.screenPng());
      console.log(`saved ${link.screen.w}x${link.screen.h}`);
      break;
    }
    case "key": link.key(Number(rest[0]), rest[1] ? rest[1].codePointAt(0)! : 0); await Bun.sleep(200); break;
    case "shell": {
      const cols = process.stdout.columns ?? 80, rows = process.stdout.rows ?? 24;
      link.onTty = (d) => process.stdout.write(d);
      const done = new Promise<void>((r) => (link.onTtyClose = r));
      link.openShell(cols, rows);
      process.stdin.setRawMode?.(true);
      process.stdin.on("data", (d) => { if (d.length === 1 && d[0] === 0x1d) { link.closeShell(); return; } link.ttyWrite(new Uint8Array(d)); });
      process.stdout.on("resize", () => link.resize(process.stdout.columns, process.stdout.rows));
      console.error("[Ctrl-] to leave]");
      await Promise.race([done, new Promise<void>((r) => process.stdin.once("end", r))]);
      break;
    }
    default: console.error("unknown command " + cmd); process.exit(2);
  }
} catch (e) {
  console.error("error:", (e as Error).message);
  process.exitCode = 1;
}
link.close();
process.exit();
