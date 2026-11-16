// A small tokenizer for the restricted C of api/palanu/*.h: identifiers, integers, string literals, the punctuation a
// declaration uses, and whole preprocessor lines. Comments are dropped; the first comment of a file is its summary.

export type TokKind = "id" | "num" | "str" | "p" | "dir";

export interface Tok {
  t: TokKind;
  v: string;
  line: number;
}

export interface Lexed {
  toks: Tok[];
  summary: string;
  errors: string[];
  idents: Set<string>; // every identifier in the file (used by the collision check)
}

const PUNCT = "(),*;";

export function lex(src: string, file: string): Lexed {
  const toks: Tok[] = [];
  const errors: string[] = [];
  const idents = new Set<string>();
  let summary = "";
  let line = 1;
  let i = 0;
  let lineStart = true;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === "\n") {
      line++;
      i++;
      lineStart = true;
      continue;
    }
    if (c === " " || c === "\t" || c === "\r") {
      i++;
      continue;
    }
    if (src.startsWith("/*", i)) {
      const end = src.indexOf("*/", i + 2);
      if (end < 0) {
        errors.push(`${file}:${line}: unterminated comment`);
        break;
      }
      const text = src.slice(i + 2, end);
      if (!summary && toks.length === 0) summary = text.replace(/^\s*\* ?/gm, "").replace(/\s+/g, " ").trim();
      line += text.split("\n").length - 1;
      i = end + 2;
      lineStart = false;
      continue;
    }
    if (src.startsWith("//", i)) {
      while (i < n && src[i] !== "\n") i++;
      continue;
    }
    if (c === "#" && lineStart) {
      let end = src.indexOf("\n", i);
      if (end < 0) end = n;
      toks.push({ t: "dir", v: src.slice(i, end).trim(), line });
      i = end;
      continue;
    }
    lineStart = false;
    if (c === '"') {
      let j = i + 1;
      while (j < n && src[j] !== '"' && src[j] !== "\n") j++;
      if (src[j] !== '"') {
        errors.push(`${file}:${line}: unterminated string literal`);
        break;
      }
      toks.push({ t: "str", v: src.slice(i + 1, j), line });
      i = j + 1;
      continue;
    }
    const rest = src.slice(i, i + 256);
    let m: RegExpExecArray | null;
    if ((m = /^[A-Za-z_][A-Za-z0-9_]*/.exec(rest))) {
      toks.push({ t: "id", v: m[0], line });
      idents.add(m[0]);
      i += m[0].length;
    } else if ((m = /^(0x[0-9a-fA-F]+|[0-9]+)/.exec(rest))) {
      toks.push({ t: "num", v: m[0], line });
      i += m[0].length;
    } else if (PUNCT.includes(c)) {
      toks.push({ t: "p", v: c, line });
      i++;
    } else {
      errors.push(`${file}:${line}: unexpected character '${c}'`);
      i++;
    }
  }
  return { toks, summary, errors, idents };
}
