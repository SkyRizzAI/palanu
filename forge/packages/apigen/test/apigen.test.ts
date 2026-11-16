// Tests for the API generator: parser rules, every annotation, the emitted outputs, collisions, check mode, and the
// ledger rule. Fixtures are small trees in a temp directory; the last test checks the real repository.
//   bun test forge/packages/apigen/test
import { expect, test } from "bun:test";
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { generate, OUTPUTS, staleOutputs, writeOutputs } from "../src/generate.ts";
import { compareLedger, parseLedger, renderLedger, type LedgerRow } from "../src/ledger.ts";
import { parseModule } from "../src/parse.ts";
import { wasmSig, type Func } from "../src/model.ts";

const REPO = resolve(import.meta.dir, "../../../..");
const ANNOT = readFileSync(join(REPO, "api/palanu/annot.h"), "utf8");

function tree(files: Record<string, string>): string {
  const root = mkdtempSync(join(tmpdir(), "apigen-"));
  const all = { "api/palanu/annot.h": ANNOT, ...files };
  for (const [p, text] of Object.entries(all)) {
    mkdirSync(dirname(join(root, p)), { recursive: true });
    writeFileSync(join(root, p), text);
  }
  return root;
}

// A module header with the given body (the guard and include are added).
const demo = (body: string) => `/* Demo module. */\n#ifndef DEMO_H\n#define DEMO_H\n#include "annot.h"\n${body}\n#endif\n`;
const GOOD = demo(`PAL_SINCE(1) PAL_WASM(demo_write) PAL_DOC("Writes.")
int32_t pal_demo_write(int32_t fd, PAL_IN(len) const void *buf, uint32_t len);`);

// Parses one module and returns its problems (not the outputs).
function problemsOf(body: string): string[] {
  return parseModule("api/palanu/demo.h", "demo", demo(body), 1).problems;
}

const fn = (body: string): Func => {
  const { mod, problems } = parseModule("api/palanu/demo.h", "demo", demo(body), 1);
  if (problems.length) throw new Error(problems.join("\n"));
  return mod.funcs[0];
};

test("a valid definition parses to a function with its annotations", () => {
  const f = fn(`PAL_SINCE(1) PAL_CAP(fs) PAL_WASM(open) PAL_DOC("Opens.")
int32_t pal_open(PAL_IN(len) const char *path, uint32_t len, int32_t flags);`);
  expect(f.cname).toBe("pal_open");
  expect(f.wasm).toBe("open");
  expect(f.cap).toBe("fs");
  expect(f.since).toBe(1);
  expect(f.params[0].inLen).toBe("len");
  expect(f.params[1].isLen).toBe(true);
  expect(wasmSig(f)).toBe("(*~i)i");
});

test("wasm signatures match the strings the runtime registered before the generator", () => {
  const sig = (decl: string) => wasmSig(fn(`PAL_SINCE(1) PAL_DOC("x") ${decl}`));
  expect(sig("int32_t pal_a(int32_t fd, PAL_IN(len) const void *buf, uint32_t len);")).toBe("(i*~)i");
  expect(sig("int32_t pal_b(PAL_OUT(len) void *buf, uint32_t len);")).toBe("(*~)i");
  expect(sig("void pal_c(int32_t code);")).toBe("(i)");
  expect(sig("int64_t pal_d(void);")).toBe("()I");
  expect(sig("int32_t pal_e(void);")).toBe("()i");
  expect(sig("int32_t pal_f(int32_t i, PAL_OUT(cap) void *buf, uint32_t cap);")).toBe("(i*~)i");
  expect(sig("int32_t pal_g(const char *name);")).toBe("($)i");
});

test("PAL_SINCE and PAL_DOC are required", () => {
  expect(problemsOf(`PAL_DOC("x")\nint32_t pal_a(void);`).join("\n")).toContain("PAL_SINCE(n) is required");
  expect(problemsOf(`PAL_SINCE(1)\nint32_t pal_a(void);`).join("\n")).toContain("PAL_DOC");
});

test("unknown annotations, bad capabilities and bad import names are rejected", () => {
  const p = problemsOf(`PAL_SINCE(1) PAL_DOC("x") PAL_CAP(gpu)\nint32_t pal_a(void);`);
  expect(p.join("\n")).toContain("PAL_CAP must be one of fs, fs.home");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x") PAL_WASM(Bad)\nint32_t pal_a(void);`).join("\n")).toContain("PAL_WASM");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x") PAL_FOO(1)\nint32_t pal_a(void);`).join("\n")).toContain("unknown annotation PAL_FOO");
});

test("the C name must be pal_ followed by lowercase letters", () => {
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t nema_a(void);`).join("\n")).toContain("must be pal_");
});

test("PAL_SINCE above the build's API level is rejected", () => {
  const { problems } = parseModule("api/palanu/demo.h", "demo", demo(`PAL_SINCE(2) PAL_DOC("x")\nint32_t pal_a(void);`), 1);
  expect(problems.join("\n")).toContain("above the API level 1");
});

test("buffers need a direction and a length right after them", () => {
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(void *buf, uint32_t len);`).join("\n")).toContain("needs PAL_OUT");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(const void *buf, uint32_t len);`).join("\n")).toContain("needs PAL_IN");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_OUT(n) void *buf, uint32_t len, uint32_t n);`).join("\n")).toContain("right after");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_OUT(len) void *buf, int32_t len);`).join("\n")).toContain("must be uint32_t");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_OUT(len) const char *s, uint32_t len);`).join("\n")).toContain("cannot be PAL_OUT");
});

test("PAL_INOUT is a void * that is read and written: it needs a length right after it, and nothing else", () => {
  const f = fn(`PAL_SINCE(1) PAL_WASM(wait) PAL_DOC("x")\nint32_t pal_demo_wait(PAL_INOUT(n_bytes) void *waitfor, uint32_t n_bytes, int32_t timeout_ms);`);
  expect(f.params[0].type).toEqual({ k: "buf", isConst: false });
  expect(f.params[0].inoutLen).toBe("n_bytes");
  expect(f.params[0].outLen).toBeUndefined();
  expect(f.params[0].inLen).toBeUndefined();
  expect(f.params[1].isLen).toBe(true);
  expect(wasmSig(f)).toBe("(*~i)i");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_INOUT(len) const void *buf, uint32_t len);`).join("\n")).toContain("needs PAL_IN(length)");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_IN(len) void *buf, uint32_t len);`).join("\n")).toContain("needs PAL_OUT(length) or PAL_INOUT(length)");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_INOUT(len) const char *s, uint32_t len);`).join("\n")).toContain("cannot be PAL_INOUT");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_INOUT(n) void *buf, uint32_t len, uint32_t n);`).join("\n")).toContain("right after");
});

test("a dotted capability is written as a string, and only known names are accepted", () => {
  const f = fn(`PAL_SINCE(1) PAL_CAP("proc.spawn") PAL_WASM(spawn) PAL_DOC("x")\nint32_t pal_a(void);`);
  expect(f.cap).toBe("proc.spawn");
  expect(problemsOf(`PAL_SINCE(1) PAL_CAP("proc.kill") PAL_DOC("x")\nint32_t pal_a(void);`).join("\n")).toContain("PAL_CAP must be one of");
});

test("pointer-to-string needs no length; a bare const char * is NUL-terminated", () => {
  const f = fn(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(const char *name);`);
  expect(f.params[0].inLen).toBeUndefined();
  expect(wasmSig(f)).toBe("($)i");
});

test("unsupported C types are rejected with the allowed list", () => {
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint pal_a(void);`).join("\n")).toContain("allowed: int32_t");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(struct foo *p);`).join("\n")).toContain("unsupported");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(char *s, uint32_t n);`).join("\n")).toContain("unsupported pointer");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nvoid *pal_a(void);`).join("\n")).toContain("cannot be returned");
});

test("PAL_IN / PAL_OUT belong on parameters only", () => {
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x") PAL_OUT(len)\nint32_t pal_a(void);`).join("\n")).toContain("belongs on a parameter");
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(PAL_IN(n) int32_t n);`).join("\n")).toContain("is for pointer parameters");
});

test("PAL_ALIAS keeps the old name as a deprecated wrapper; PAL_REMOVED keeps the row only", () => {
  const root = tree({
    "api/palanu/demo.h": demo(`PAL_SINCE(1) PAL_WASM(demo_old) PAL_ALIAS(pal_demo_old) PAL_DOC("Old.")
int32_t pal_demo_new(int32_t a);
PAL_SINCE(1) PAL_REMOVED(3) PAL_WASM(demo_gone) PAL_DOC("Gone.")
void pal_demo_gone(void);`),
  });
  const g = generate(root);
  expect(g.problems).toEqual([]);
  const sdk = g.files.get(OUTPUTS.sdk)!;
  expect(sdk).toContain('__attribute__((deprecated("use pal_demo_new"))) static inline int32_t pal_demo_old(int32_t a) { return pal_demo_new(a); }');
  expect(sdk).not.toContain("pal_demo_gone");
  expect(g.files.get(OUTPUTS.glue)!).not.toContain("demo_gone");
  const ledger = g.files.get(OUTPUTS.ledger)!;
  expect(ledger).toContain("pal_demo_old,1,int32_t(int32_t),,demo_old,deprecated");
  expect(ledger).toContain("pal_demo_gone,1,void(),,demo_gone,removed");
});

test("constants come from #define lines with an integer value", () => {
  const root = tree({ "api/palanu/demo.h": demo(`#define PAL_DEMO_A 0x01\n#define PAL_DEMO_B (-7)\n${GOOD.split("#include \"annot.h\"\n")[1].split("#endif")[0]}`) });
  const g = generate(root);
  expect(g.problems).toEqual([]);
  expect(g.files.get(OUTPUTS.sdk)!).toContain("#define PAL_DEMO_A 0x01");
  expect(g.files.get(OUTPUTS.sdk)!).toContain("#define PAL_DEMO_B (-7)");
  expect(problemsOf(`#define PAL_DEMO_C something\n`).join("\n")).toContain("the value must be an integer");
});

test("collision: a new public name that a system header declares fails the build", () => {
  const root = tree({
    "api/palanu/demo.h": GOOD,
    "system/include/pal/clash.h": "int pal_demo_write(void);\n",
  });
  const g = generate(root);
  expect(g.files.size).toBe(0);
  expect(g.problems.join("\n")).toContain("collision: pal_demo_write is already declared in system/include/pal/clash.h");
});

test("an old alias name that a system header declares only warns", () => {
  const root = tree({
    "api/palanu/demo.h": demo(`PAL_SINCE(1) PAL_WASM(demo_write) PAL_ALIAS(pal_puts) PAL_DOC("x")\nint32_t pal_demo_write(int32_t fd);`),
    "apps/libapp.h": "int pal_puts(const char *s);\n",
  });
  const g = generate(root);
  expect(g.problems).toEqual([]);
  expect(g.warnings.join("\n")).toContain("old name pal_puts is also declared in apps/libapp.h");
});

test("names must be unique across modules and wasm import names too", () => {
  const root = tree({
    "api/palanu/demo.h": GOOD,
    "api/palanu/other.h": demo(`PAL_SINCE(1) PAL_WASM(demo_write) PAL_DOC("y")\nint32_t pal_other(void);`),
  });
  expect(generate(root).problems.join("\n")).toContain('import name "demo_write" is already defined');
});

test("the parameter names c, ctx and env are reserved for the generated code", () => {
  expect(problemsOf(`PAL_SINCE(1) PAL_DOC("x")\nint32_t pal_a(int32_t c);`).join("\n")).toContain("reserved");
});

test("the API level must agree with the firmware's papp.h", () => {
  const level = /PAL_API_LEVEL\s+(\d+)/.exec(ANNOT)![1];
  const other = Number(level) + 1;
  const root = tree({
    "api/palanu/demo.h": GOOD,
    "system/include/pal/papp.h": `#define PAL_PAPP_API ${other}\n`,
  });
  expect(generate(root).problems.join("\n")).toContain(`PAL_PAPP_API ${other} differs from PAL_API_LEVEL ${level}`);
});

test("write then check: an edited output is reported stale, a fresh one is not", () => {
  const root = tree({ "api/palanu/demo.h": GOOD });
  const g = generate(root);
  writeOutputs(root, g.files);
  expect(staleOutputs(root, g.files)).toEqual([]);
  writeFileSync(join(root, OUTPUTS.sdk), "edited\n");
  expect(staleOutputs(root, g.files)).toEqual([OUTPUTS.sdk]);
});

test("the ledger rule: a changed signature, cap, or import name fails; added rows pass; removal fails unless removed", () => {
  const base: LedgerRow[] = [
    { name: "pal_a", since: 1, signature: "int32_t(int32_t)", cap: "", wasm_name: "a", status: "stable" },
    { name: "pal_b", since: 1, signature: "void()", cap: "fs", wasm_name: "b", status: "stable" },
    { name: "pal_c", since: 1, signature: "void()", cap: "", wasm_name: "c", status: "stable" },
  ];
  const head = renderLedger(base);
  expect(compareLedger(head, renderLedger(base))).toEqual([]);
  const added = [...base, { name: "pal_d", since: 2, signature: "void()", cap: "", wasm_name: "d", status: "stable" as const }];
  expect(compareLedger(head, renderLedger(added))).toEqual([]);
  const sig = base.map((r) => (r.name === "pal_a" ? { ...r, signature: "int64_t(int32_t)" } : r));
  expect(compareLedger(head, renderLedger(sig))).toEqual(["api: pal_a changed signature (use a new name)"]);
  const cap = base.map((r) => (r.name === "pal_b" ? { ...r, cap: "" } : r));
  expect(compareLedger(head, renderLedger(cap))[0]).toContain("pal_b changed cap");
  const wasm = base.map((r) => (r.name === "pal_c" ? { ...r, wasm_name: "cc" } : r));
  expect(compareLedger(head, renderLedger(wasm))[0]).toContain("pal_c changed wasm_name");
  const gone = base.filter((r) => r.name !== "pal_c");
  expect(compareLedger(head, renderLedger(gone))).toEqual(["api: pal_c removed (keep its row and set status=removed)"]);
  const removed = base.map((r) => (r.name === "pal_c" ? { ...r, status: "removed" as const } : r));
  expect(compareLedger(head, renderLedger(removed))).toEqual([]);
  expect(compareLedger("", renderLedger(base))).toEqual([]); // no committed ledger yet: nothing to compare
});

test("the ledger CSV round-trips signatures with commas", () => {
  const rows: LedgerRow[] = [{ name: "pal_x", since: 1, signature: "int32_t(int32_t, const void *, uint32_t)", cap: "fs", wasm_name: "x", status: "stable" }];
  expect(parseLedger(renderLedger(rows)).rows).toEqual(rows);
});

test("the checked-in outputs are generated from the checked-in definitions", () => {
  const g = generate(REPO);
  expect(g.problems).toEqual([]);
  expect(staleOutputs(REPO, g.files)).toEqual([]);
  expect(g.files.get(OUTPUTS.ledger)!).toContain("pal_write,1,");
});
