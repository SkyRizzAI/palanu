// Types shared by the parser and the emitters. A definition is a C function declaration in api/palanu/<module>.h
// (see api/palanu/annot.h for the annotations). Nothing here knows about WAMR or the system; the emitters do.

export const WASM_MODULE = "palanu"; // the import module every app uses
export const KNOWN_CAPS = ["fs", "fs.home", "fs.media", "net", "net.listen", "proc.spawn", "crypto.keys", "settings.read", "settings.write", "notify", "dev"]; // manifest permissions (system/include/pal/papp.h)
export const SCALARS = ["int32_t", "uint32_t", "int64_t", "uint64_t", "uint8_t", "pal_handle_t"] as const;

// The C types a definition may use. A buffer is always a pointer plus a length parameter (PAL_IN / PAL_OUT).
export type CType =
  | { k: "void" }
  | { k: "scalar"; t: string }
  | { k: "cstr" } // const char *
  | { k: "buf"; isConst: boolean }; // void * (output) or const void * (input)

export interface Param {
  name: string;
  type: CType;
  line: number;
  inLen?: string; // this pointer is input; the named parameter is its length
  outLen?: string; // this pointer is an output buffer; the named parameter is its length
  inoutLen?: string; // PAL_INOUT: the buffer is read and written; the named parameter is its length
  isLen?: boolean; // this parameter is the length of the pointer before it
}

export interface Func {
  module: string;
  file: string;
  line: number;
  cname: string; // the name apps call in the SDK, pal_<module>_... or short (pal_write)
  ret: CType;
  params: Param[];
  since: number;
  doc: string;
  cap?: string;
  wasm: string; // wasm import name
  alias?: string; // deprecated old C name kept in the SDK
  removed?: number; // API level that removed it
}

export interface Const {
  module: string;
  file: string;
  line: number;
  name: string;
  value: string;
}

export interface ModuleDef {
  name: string; // file basename: fs, io, proc, time, tty
  file: string;
  summary: string; // first comment of the file
  funcs: Func[];
  consts: Const[];
}

// A problem is one line of text, "file:line: what is wrong", printed as it is.
export type Problem = string;

export function ctype(t: CType, glue: boolean): string {
  switch (t.k) {
    case "void":
      return "void";
    case "scalar":
      return glue && t.t === "pal_handle_t" ? "int32_t" : t.t;
    case "cstr":
      return "const char *";
    case "buf":
      return t.isConst ? "const void *" : "void *";
  }
}

// C declarator text for a parameter: "const char *path", "uint32_t len".
export function declare(name: string, t: CType, glue: boolean): string {
  const text = ctype(t, glue);
  return text.endsWith("*") ? `${text}${name}` : `${text} ${name}`;
}

// WAMR signature character for a scalar: i for 32-bit, I for 64-bit.
export function wasmScalar(t: CType): string {
  return t.k === "scalar" && (t.t === "int64_t" || t.t === "uint64_t") ? "I" : "i";
}

// The WAMR native signature (system/src/api_wasm_glue.c): "*~" is a pointer with the length after it, "$" a
// NUL-terminated string, "i"/"I" scalars. Matches the strings the runtime registered before this generator existed.
export function wasmSig(f: Func): string {
  let s = "(";
  for (const p of f.params) {
    if (p.isLen) continue;
    if (p.type.k === "buf" || (p.type.k === "cstr" && p.inLen !== undefined)) s += "*~";
    else if (p.type.k === "cstr") s += "$";
    else s += wasmScalar(p.type);
  }
  s += ")";
  if (f.ret.k !== "void") s += wasmScalar(f.ret);
  return s;
}

// Canonical C type of a function without parameter names, for the ledger: "int32_t(int32_t, const void *, uint32_t)".
export function ledgerSignature(f: Func): string {
  return `${ctype(f.ret, false)}(${f.params.map((p) => ctype(p.type, false)).join(", ")})`;
}

export function apiFuncs(mods: ModuleDef[]): Func[] {
  return mods.flatMap((m) => m.funcs);
}
