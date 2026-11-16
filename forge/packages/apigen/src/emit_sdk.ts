// sdk/palanu.h: the header an app includes. Freestanding wasm32 C: the imports, the constants, and three helpers.
import { WASM_MODULE, type Func, type ModuleDef, ctype, declare } from "./model.ts";

const HEADER_NOTE = `/* PalanuOS app SDK. Freestanding C for wasm32: no libc, so everything the app can do is below.
 *
 *   clang --target=wasm32 -mcpu=mvp -mbulk-memory -O2 -ffreestanding -nostdlib -c app.c -o app.o
 *   rust-lld -flavor wasm --no-entry --export=_start app.o -o app.wasm
 *   palanu pack --id myapp --name "My App" --ver 0.1 app.wasm -o myapp.papp
 *
 * or just \`bun forge/tools/palanu.ts build app.c --id myapp\` from the repo root (same steps).
 * The module exports \`_start\`; its return ends the app. */`;

const HELPERS = `/* Helpers (not imports): text output on top of pal_write. */
static inline unsigned pal_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static inline int32_t pal_puts(const char *s) { return pal_write(1, s, pal_strlen(s)); }
static inline void pal_putu(unsigned long long v) {
    char b[24];
    int i = 24;
    do b[--i] = (char)('0' + v % 10), v /= 10;
    while (v);
    pal_write(1, b + i, (unsigned)(24 - i));
}`;

/* The call's parameter list as the SDK declares it: "const char *path, uint32_t len, int32_t flags", or "void". */
function paramList(f: Func): string {
  return f.params.length ? f.params.map((p) => declare(p.name, p.type, false)).join(", ") : "void";
}

function capNote(f: Func): string {
  return f.cap ? `, needs the "${f.cap}" permission` : "";
}

function funcBlock(f: Func): string[] {
  const out = [`/* ${f.cname} (since ${f.since}${capNote(f)}): ${f.doc} */`];
  out.push(`PAL_IMPORT(${f.wasm}) ${ctype(f.ret, false)} ${f.cname}(${paramList(f)});`);
  if (f.alias) {
    const args = f.params.map((p) => p.name).join(", ");
    const ret = f.ret.k === "void" ? "" : "return ";
    out.push(
      `/* ${f.alias}: the older name of ${f.cname}, kept for old sources. */`,
      `__attribute__((deprecated("use ${f.cname}"))) static inline ${ctype(f.ret, false)} ${f.alias}(${paramList(f)}) { ${ret}${f.cname}(${args}); }`,
    );
  }
  return out;
}

export function emitSdk(mods: ModuleDef[], level: number): string {
  const out: string[] = [
    HEADER_NOTE,
    "/* GENERATED from the api/palanu headers by forge/packages/apigen. Do not edit: change the definitions and run",
    " * `bun forge/packages/apigen/src/cli.ts`. Reference: docs/API.md. */",
    "#ifndef PALANU_H",
    "#define PALANU_H",
    "",
    "#include <stdint.h>",
    "",
    `#define PAL_API ${level}`,
    `#define PAL_IMPORT(n) __attribute__((import_module("${WASM_MODULE}"), import_name(#n)))`,
    "",
    "typedef int32_t pal_handle_t;",
  ];
  for (const m of mods) {
    out.push("", `/* ---- ${m.name}: ${m.summary} ---- */`);
    for (const c of m.consts) out.push(`#define ${c.name} ${c.value}`);
    for (const f of m.funcs) {
      if (f.removed !== undefined) continue;
      out.push("", ...funcBlock(f));
    }
  }
  out.push("", HELPERS, "", "#endif", "");
  return out.join("\n");
}
