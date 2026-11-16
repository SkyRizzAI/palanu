#!/usr/bin/env bun
// Internal RAM budget of the firmware (run by `./pal build` and `./pal flash` after the link; needs the ESP-IDF tools on PATH).
//
// Wi-Fi and BLE need about 75 KB of free internal RAM each to start, and the two must run together (docs/SIZE-BUDGETS.md,
// docs/adr/0002). A static footprint that grows a little with every screen eventually leaves no room for the radios.
// PalanuOS keeps static data out of internal RAM by rule: a zero-initialised static of 1 KB or more in system, apps, nema,
// platform or boards is written `NEMA_EXT_BSS` (nema/include/nema/types.h) and lives in PSRAM. The few that must stay
// internal are listed in ALLOWED with the reason. The build fails when
//   1. one of our statics of MIN_SIZE bytes or more sits in internal RAM and is not in ALLOWED, or
//   2. our statics in internal RAM add up to more than OWN_BUDGET, or
//   3. all .bss and .data in internal RAM (ours, ESP-IDF's, the libraries') add up to more than TOTAL_BUDGET.
// Raise a budget only with a measurement in docs/SIZE-BUDGETS.md showing Wi-Fi and BLE still start together.
//
// Stacks too (the components build with -fstack-usage): the desktop task has 6 KB, the supervisor 6 KB, and a screen's draw
// or key handler runs there under the file system and the display. A function whose own frame is above STACK_MAX fails
// the build, unless it is a shell command (cmd_*, which runs on a shell's 8 to 12 KB stack, limit CMD_STACK_MAX) or it is
// in STACK_ALLOWED with the reason. The applist's 6 KB fixture array on the desktop stack rebooted the board (2026-11-14).
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "../..");
const ELF = join(ROOT, "targets/hhv2/build/palanuos.elf");
const NM = "riscv32-esp-elf-nm";

const MIN_SIZE = 1024;
const OWN_BUDGET = 40 * 1024; /* 38.7 KB on 2026-11-14 */
const TOTAL_BUDGET = 80 * 1024; /* 73.3 KB on 2026-11-14 */
/* Internal RAM on the ESP32-S31: DRAM is mapped at 0x2F000000 (data and bss). PSRAM, flash and RTC RAM are elsewhere. */
const INTERNAL = (addr: number) => addr >= 0x2f000000 && addr < 0x2f100000;
const OURS = ["system/", "apps/", "nema/", "platform/", "boards/"];

/* "file.c:symbol": why it stays in internal RAM. */
const ALLOWED: Record<string, string> = {
  "nema/src/proc.c:P": "kernel process table, read on every syscall and by the scheduler",
  "nema/src/mem.c:M": "kernel allocator state, used while PSRAM may be busy",
  "nema/src/task.c:T": "kernel task table",
  "nema/src/vfs.c:V": "kernel mount and handle table",
  "platform/esp-idf/palanu/port_esp.c:tasks": "FreeRTOS task records of the port",
  "system/api/kv.c:s_idx": "kv index, used on the flash write path",
  "system/api/kv.c:s_rec": "kv record buffer, written to flash",
  "system/api/kv.c:s_out": "kv record buffer, read from flash",
  "system/src/api_wasm_glue.c:api_palanu_natives": "WAMR native table: initialised and written by the runtime at registration",
  "system/src/vk.c:vk_S": "on-screen keyboard state with initialisers (1.5 KB)",
};

const STACK_MAX = 1536;
const CMD_STACK_MAX = 4096;
const STACK_DIRS = ["nema", "system", "apps", "palanu"].map((d) => join(ROOT, "targets/hhv2/build/esp-idf", d));
/* "file.c:function": why its frame may be larger. Third-party code (third_party/) is not checked. */
const STACK_ALLOWED: Record<string, string> = {
  "system/src/keyfmt.c:kf_import_openssh": "key import: the keys API from an app (12 KB native stack) and ssh-keygen (shell)",
  "system/src/cryptoimpl_mbedtls.c:ci_rsa_verify_pkcs1": "RSA verify: the crypto API from an app (12 KB) and ssh (shell)",
};

function suFiles(dir: string, out: string[]) {
  if (!existsSync(dir)) return;
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) suFiles(p, out);
    else if (e.endsWith(".su")) out.push(p);
  }
}

function stackProblems(): string[] {
  const files: string[] = [];
  for (const d of STACK_DIRS) suFiles(d, files);
  if (files.length === 0) return ["  no .su files: the components must build with -fstack-usage"];
  const out: string[] = [];
  for (const f of files) {
    for (const ln of readFileSync(f, "utf8").split("\n")) {
      const [where, size] = ln.split("\t");
      if (!where || !size) continue;
      const rel = where.replace(ROOT + "/", "");
      if (rel.startsWith("third_party/")) continue;
      const parts = rel.split(":"); /* file:line:col:function */
      const file = parts[0], fn = (parts[3] ?? "").replace(/\..*$/, ""); /* cmd_list.isra -> cmd_list */
      const n = parseInt(size, 10);
      const limit = fn.startsWith("cmd_") ? CMD_STACK_MAX : STACK_MAX;
      if (n <= limit || STACK_ALLOWED[`${file}:${fn}`]) continue;
      out.push(`  ${file}:${parts[1]} ${fn}: a ${n} B stack frame (limit ${limit}): move the big locals to the heap or a static`);
    }
  }
  return out;
}

type Sym = { size: number; kind: string; name: string; file: string; line: string };

function symbols(): Sym[] {
  const p = Bun.spawnSync([NM, "-S", "--size-sort", "-l", ELF], { stdout: "pipe", stderr: "pipe" });
  if (p.exitCode !== 0) throw new Error(`${NM} failed: ${p.stderr.toString().trim()}`);
  const out: Sym[] = [];
  for (const ln of p.stdout.toString().split("\n")) {
    const [head, loc = ""] = ln.split("\t");
    const f = head.trim().split(/\s+/);
    if (f.length < 4 || !"bBdD".includes(f[2])) continue;
    if (!INTERNAL(parseInt(f[0], 16))) continue;
    const where = loc.replace(ROOT + "/", "");
    const colon = where.lastIndexOf(":");
    out.push({
      size: parseInt(f[1], 16),
      kind: f[2],
      name: f[3].replace(/\.\d+$/, ""), /* function-local statics: buf.0 -> buf */
      file: colon > 0 ? where.slice(0, colon) : where,
      line: colon > 0 ? where.slice(colon + 1) : "",
    });
  }
  return out;
}

function main(): number {
  if (!existsSync(ELF)) {
    console.error(`ramcheck: ${ELF} not found; build first`);
    return 1;
  }
  const syms = symbols();
  const ours = syms.filter((s) => OURS.some((d) => s.file.startsWith(d)));
  const own = ours.reduce((a, s) => a + s.size, 0);
  const total = syms.reduce((a, s) => a + s.size, 0);
  const problems: string[] = [];
  for (const s of ours) {
    if (s.size < MIN_SIZE || ALLOWED[`${s.file}:${s.name}`]) continue;
    const how = s.kind === "d" || s.kind === "D" ? "drop the initialiser (set it in code) and add NEMA_EXT_BSS" : "add NEMA_EXT_BSS";
    problems.push(`  ${s.file}:${s.line} ${s.name} (${s.size} B in internal RAM): ${how}, or list it in ALLOWED with the reason`);
  }
  if (own > OWN_BUDGET) problems.push(`  our statics in internal RAM: ${own} B, budget ${OWN_BUDGET} B`);
  if (total > TOTAL_BUDGET) problems.push(`  all .bss and .data in internal RAM: ${total} B, budget ${TOTAL_BUDGET} B`);
  problems.push(...stackProblems());
  const kb = (n: number) => `${(n / 1024).toFixed(1)} KB`;
  console.log(`ramcheck: internal RAM statics ${kb(total)} of ${kb(TOTAL_BUDGET)}; PalanuOS's ${kb(own)} of ${kb(OWN_BUDGET)}`);
  if (problems.length) {
    console.error(`ramcheck: ${problems.length} problem(s); internal RAM and task stacks are kept in budget:\n${problems.join("\n")}`);
    return 1;
  }
  return 0;
}

process.exit(main());
