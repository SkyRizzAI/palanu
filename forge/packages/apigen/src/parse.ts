// Parses one module header (api/palanu/<module>.h) into functions and constants. Every rule the generator relies on is
// checked here, so the emitters can trust their input. Problems are collected, not thrown, so one run reports them all.
import { lex, type Tok } from "./lex.ts";
import { type CType, type Const, type Func, type ModuleDef, type Param, type Problem, KNOWN_CAPS, SCALARS } from "./model.ts";

const NAME_RE = /^pal_[a-z][a-z0-9_]*$/;
const WASM_RE = /^[a-z][a-z0-9_]*$/;
const CONST_VALUE_RE = /^\(?-?(0x[0-9a-fA-F]+|[0-9]+)\)?$/;
const FUNC_ANNOTATIONS = new Set(["PAL_SINCE", "PAL_DOC", "PAL_CAP", "PAL_WASM", "PAL_ALIAS", "PAL_REMOVED"]);
const PARAM_ANNOTATIONS = new Set(["PAL_IN", "PAL_OUT", "PAL_INOUT"]);
const ALL_ANNOTATIONS = new Set([...FUNC_ANNOTATIONS, ...PARAM_ANNOTATIONS]);
// Names the generated code uses for itself: the implementation context and the glue's locals.
const RESERVED_PARAMS = new Set(["c", "ctx", "env", "r"]);

interface Ann {
  name: string;
  arg: Tok;
  line: number;
}

export interface ParsedModule {
  mod: ModuleDef;
  problems: Problem[];
  idents: Set<string>; // every identifier in the header (collision check)
}

/* `file` is the path shown in messages, `name` the module (file basename), `level` the API level of the build. */
export function parseModule(file: string, name: string, src: string, level: number): ParsedModule {
  const problems: Problem[] = [];
  const err = (line: number, message: string) => problems.push(`${file}:${line}: ${message}`);
  const lexed = lex(src, file);
  problems.push(...lexed.errors);
  const mod: ModuleDef = { name, file, summary: lexed.summary, funcs: [], consts: [] };

  // Reads PAL_X(arg) annotations starting at i; returns the index after them.
  const readAnnotations = (toks: Tok[], start: number, out: Ann[]): number => {
    let i = start;
    while (i < toks.length && toks[i].t === "id" && toks[i].v.startsWith("PAL_") && toks[i + 1]?.v === "(") {
      const line = toks[i].line;
      const name = toks[i].v;
      if (toks[i + 2]?.t === undefined || toks[i + 2].t === "p" || toks[i + 3]?.v !== ")") {
        err(line, `${name} takes exactly one argument`);
        while (i < toks.length && toks[i].v !== ")") i++;
        i++;
        continue;
      }
      out.push({ name, arg: toks[i + 2], line });
      i += 4;
    }
    return i;
  };

  // A C type from tokens. Only the forms the ABI can express: scalars, const char * (a string), void * and const void *
  // (a buffer, which needs a length), and void as a return type.
  const typeOf = (toks: Tok[], line: number, isReturn: boolean): CType | undefined => {
    const text = toks.map((t) => t.v).join(" ");
    let t = toks;
    const isConst = t[0]?.v === "const";
    if (isConst) t = t.slice(1);
    const star = t.length > 0 && t[t.length - 1].v === "*";
    if (star) t = t.slice(0, -1);
    if (t.length !== 1 || t[0].t !== "id") return fail(line, `unsupported type "${text}"`);
    const base = t[0].v;
    if (star) {
      if (isReturn) return fail(line, `a pointer cannot be returned: "${text}"`);
      if (base === "void") return { k: "buf", isConst };
      if (base === "char" && isConst) return { k: "cstr" };
      return fail(line, `unsupported pointer "${text}" (use const char * or void * / const void *)`);
    }
    if (isConst) return fail(line, `const applies only to pointers: "${text}"`);
    if (base === "void") return isReturn ? { k: "void" } : fail(line, "a void parameter must be the only one");
    if ((SCALARS as readonly string[]).includes(base)) return { k: "scalar", t: base };
    return fail(line, `unsupported type "${base}" (allowed: ${SCALARS.join(", ")})`);
  };
  const fail = (line: number, message: string): undefined => {
    err(line, message);
    return undefined;
  };

  // The parameters of one declaration, with their PAL_IN / PAL_OUT annotations checked and linked to their length.
  const parseParams = (toks: Tok[], line: number): Param[] | undefined => {
    const groups: Tok[][] = [];
    let depth = 0;
    let cur: Tok[] = [];
    for (const t of toks) {
      if (t.t === "p" && t.v === "(") depth++;
      if (t.t === "p" && t.v === ")") depth--;
      if (t.t === "p" && t.v === "," && depth === 0) {
        groups.push(cur);
        cur = [];
      } else cur.push(t);
    }
    if (cur.length) groups.push(cur);
    if (groups.length === 1 && groups[0].length === 1 && groups[0][0].v === "void") return [];
    const params: Param[] = [];
    const anns: Ann[][] = [];
    let ok = true;
    for (const g of groups) {
      const pa: Ann[] = [];
      const j = readAnnotations(g, 0, pa);
      const rest = g.slice(j);
      const pline = g[0]?.line ?? line;
      if (rest.length < 2 || rest[rest.length - 1].t !== "id") {
        err(pline, "a parameter needs a type and a name");
        ok = false;
        continue;
      }
      const pname = rest[rest.length - 1].v;
      if (RESERVED_PARAMS.has(pname)) {
        err(pline, `parameter name "${pname}" is reserved by the generated code`);
        ok = false;
      }
      if (params.some((p) => p.name === pname)) {
        err(pline, `duplicate parameter "${pname}"`);
        ok = false;
      }
      const type = typeOf(rest.slice(0, -1), pline, false);
      if (!type) {
        ok = false;
        continue;
      }
      for (const a of pa) {
        if (PARAM_ANNOTATIONS.has(a.name)) continue;
        err(a.line, FUNC_ANNOTATIONS.has(a.name) ? `${a.name} belongs on a function, not a parameter` : `unknown annotation ${a.name}`);
      }
      params.push({ name: pname, type, line: pline });
      anns.push(pa);
    }
    // Pointers: each needs the right direction and a length parameter right after it.
    params.forEach((p, k) => {
      const pa = anns[k];
      const dir = pa.find((a) => PARAM_ANNOTATIONS.has(a.name));
      const ptr = p.type.k === "buf" || p.type.k === "cstr";
      if (dir && !ptr) return err(p.line, `${dir.name} is for pointer parameters; "${p.name}" is not one`);
      if (p.type.k === "buf") {
        if (p.type.isConst && (!dir || dir.name !== "PAL_IN")) return err(p.line, `const void * "${p.name}" needs PAL_IN(length)`);
        if (!p.type.isConst && (!dir || (dir.name !== "PAL_OUT" && dir.name !== "PAL_INOUT"))) return err(p.line, `void * "${p.name}" needs PAL_OUT(length) or PAL_INOUT(length)`);
      }
      if (p.type.k === "cstr" && dir && dir.name !== "PAL_IN") return err(p.line, `const char * "${p.name}" cannot be ${dir.name}`);
      if (!dir) return;
      const lenName = dir.arg.v;
      const lenIdx = params.findIndex((q) => q.name === lenName);
      if (dir.arg.t !== "id" || lenIdx !== k + 1) {
        return err(dir.line, `${dir.name}(${lenName}): the length must be the parameter right after "${p.name}"`);
      }
      const lenType = params[lenIdx].type;
      if (lenType.k !== "scalar" || lenType.t !== "uint32_t") return err(dir.line, `length "${lenName}" must be uint32_t`);
      if (dir.name === "PAL_IN") p.inLen = lenName;
      else if (dir.name === "PAL_OUT") p.outLen = lenName;
      else p.inoutLen = lenName; // PAL_INOUT: read on the way in and written on the way out, e.g. pal_wait records
      params[lenIdx].isLen = true;
    });
    return ok ? params : undefined;
  };

  // A declaration: annotations, return type, name, parameters, ";".
  const declaration = (toks: Tok[]): void => {
    if (toks.length === 0) return;
    const line = toks[0].line;
    const anns: Ann[] = [];
    const i = readAnnotations(toks, 0, anns);
    const open = toks.findIndex((t, k) => k >= i && t.t === "p" && t.v === "(");
    const close = toks.length - 1;
    if (open < 0 || i === open || toks[open - 1].t !== "id" || toks[close].v !== ")") {
      return err(line, "not a function declaration (expected: annotations, type, name, parameters, ';')");
    }
    const cname = toks[open - 1].v;
    const ret = typeOf(toks.slice(i, open - 1), line, true);
    const params = parseParams(toks.slice(open + 1, close), line);
    if (!ret || !params) return;
    const fileAnn = (n: string) => anns.filter((a) => a.name === n);
    for (const a of anns) {
      if (!FUNC_ANNOTATIONS.has(a.name)) err(a.line, PARAM_ANNOTATIONS.has(a.name) ? `${a.name} belongs on a parameter` : `unknown annotation ${a.name}`);
      if (fileAnn(a.name).length > 1) err(a.line, `${a.name} given twice on ${cname}`);
    }
    if (!NAME_RE.test(cname)) err(line, `"${cname}": the name must be pal_ followed by lowercase letters, digits or _`);
    const one = (n: string) => fileAnn(n)[0];
    const sinceA = one("PAL_SINCE");
    const docA = one("PAL_DOC");
    const capA = one("PAL_CAP");
    const wasmA = one("PAL_WASM");
    const aliasA = one("PAL_ALIAS");
    const removedA = one("PAL_REMOVED");
    if (!sinceA) err(line, `${cname}: PAL_SINCE(n) is required`);
    if (!docA) err(line, `${cname}: PAL_DOC("...") is required`);
    const since = sinceA && sinceA.arg.t === "num" ? Number(sinceA.arg.v) : NaN;
    if (sinceA && !(since >= 1)) err(sinceA.line, `${cname}: PAL_SINCE takes an API level >= 1`);
    if (sinceA && since > level) err(sinceA.line, `${cname}: PAL_SINCE(${since}) is above the API level ${level} (annot.h)`);
    const doc = docA && docA.arg.t === "str" ? docA.arg.v.trim() : "";
    if (docA && (!doc || doc.includes("*/") || doc.includes('"'))) err(docA.line, `${cname}: PAL_DOC must be a non-empty string without */ or "`);
    let cap: string | undefined;
    if (capA) {
      // fs and net are identifiers; the dotted ones (fs.home, net.listen) are written as strings: PAL_CAP("net.listen")
      const name = capA.arg.t === "id" || capA.arg.t === "str" ? capA.arg.v : "";
      if (!KNOWN_CAPS.includes(name)) err(capA.line, `${cname}: PAL_CAP must be one of ${KNOWN_CAPS.join(", ")}`);
      else cap = name;
    }
    let wasm = cname;
    if (wasmA) {
      if (wasmA.arg.t !== "id" || !WASM_RE.test(wasmA.arg.v)) err(wasmA.line, `${cname}: PAL_WASM takes a lowercase import name`);
      else wasm = wasmA.arg.v;
    }
    let alias: string | undefined;
    if (aliasA) {
      if (aliasA.arg.t !== "id" || !NAME_RE.test(aliasA.arg.v)) err(aliasA.line, `${cname}: PAL_ALIAS takes a pal_ name`);
      else alias = aliasA.arg.v;
    }
    let removed: number | undefined;
    if (removedA) {
      if (removedA.arg.t !== "num") err(removedA.line, `${cname}: PAL_REMOVED takes an API level`);
      else {
        removed = Number(removedA.arg.v);
        if (removed < since) err(removedA.line, `${cname}: removed before it was introduced`);
        if (alias) err(aliasA!.line, `${cname}: a removed function has no alias`);
      }
    }
    if (!sinceA || !docA || !(since >= 1)) return;
    const f: Func = { module: name, file, line, cname, ret, params, since, doc, wasm };
    if (cap) f.cap = cap;
    if (alias) f.alias = alias;
    if (removed !== undefined) f.removed = removed;
    mod.funcs.push(f);
  };

  // A directive: include guards, #include, and #define NAME VALUE for constants (error codes, open flags).
  const directive = (tok: Tok): void => {
    const text = tok.v.replace(/\/\/.*$/, "").trim();
    if (/^#\s*(ifndef|endif|include|pragma)\b/.test(text)) return;
    let m = /^#\s*define\s+(\w+)$/.exec(text);
    if (m) {
      if (!m[1].endsWith("_H")) err(tok.line, `#define ${m[1]} without a value (only include guards may omit it)`);
      return;
    }
    m = /^#\s*define\s+(\w+)\s+(.+)$/.exec(text);
    if (m) {
      const [, cname, value] = m;
      if (!cname.startsWith("PAL_") || ALL_ANNOTATIONS.has(cname)) err(tok.line, `constant "${cname}" must start with PAL_ and not be an annotation`);
      else if (!CONST_VALUE_RE.test(value.trim())) err(tok.line, `constant ${cname}: the value must be an integer, e.g. 0x01 or (-7)`);
      else {
        const c: Const = { module: name, file, line: tok.line, name: cname, value: value.trim() };
        mod.consts.push(c);
      }
      return;
    }
    err(tok.line, `unsupported preprocessor line: ${text}`);
  };

  let decl: Tok[] = [];
  for (const tok of lexed.toks) {
    if (tok.t === "dir") {
      if (decl.length) {
        err(tok.line, "preprocessor line inside a declaration");
        decl = [];
      }
      directive(tok);
      continue;
    }
    if (tok.t === "p" && tok.v === ";") {
      declaration(decl);
      decl = [];
      continue;
    }
    decl.push(tok);
  }
  if (decl.length) err(decl[0].line, "declaration without ';'");
  return { mod, problems, idents: lexed.idents };
}
