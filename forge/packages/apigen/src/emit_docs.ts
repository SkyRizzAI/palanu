// docs/API.md: the reference for app authors, generated from the same definitions as the SDK header.
import { WASM_MODULE, type Func, type ModuleDef, declare, ctype, ledgerSignature } from "./model.ts";

const cell = (s: string) => s.replace(/\|/g, "\\|");

function funcSection(f: Func): string[] {
  const proto = `${ctype(f.ret, false)} ${f.cname}(${f.params.length ? f.params.map((p) => declare(p.name, p.type, false)).join(", ") : "void"});`;
  const meta = [`import \`${f.wasm}\``, `since ${f.since}`];
  if (f.cap) meta.push(`needs \`${f.cap}\``);
  if (f.alias) meta.push(`old name \`${f.alias}\``);
  const out = [`#### \`${f.cname}\``, "", "```c", proto, "```", "", `${f.doc}`, "", meta.join(" · ")];
  if (f.removed !== undefined) out.push("", `**Removed in API level ${f.removed}.** Apps built for it fail to load.`);
  out.push("");
  return out;
}

export function emitDocs(mods: ModuleDef[], level: number): string {
  const out: string[] = [
    "# PalanuOS app API",
    "",
    "Generated from `api/palanu/*.h` by `forge/packages/apigen`. Do not edit it by hand: change the definitions,",
    "then run `bun forge/packages/apigen/src/cli.ts`. `./pal check` rejects a file that is out of sync.",
    "",
    `API level: **${level}**. WebAssembly import module: \`${WASM_MODULE}\`. SDK header: \`sdk/palanu.h\`.`,
    "The full list of names, signatures and permissions is in `api/symbols.csv`; `./pal check` enforces the change rules.",
    "",
    "## Modules",
    "",
    "| Module | Contents |",
    "|---|---|",
    ...mods.map((m) => `| \`${m.name}\` | ${cell(m.summary)} |`),
    "",
  ];
  for (const m of mods) {
    out.push(`## ${m.name}`, "", m.summary, "");
    if (m.consts.length) {
      out.push("### Constants", "", "| Name | Value |", "|---|---|");
      for (const c of m.consts) out.push(`| \`${c.name}\` | \`${c.value}\` |`);
      out.push("");
    }
    const live = m.funcs.filter((f) => f.removed === undefined);
    if (live.length) out.push("### Functions", "");
    for (const f of live) out.push(...funcSection(f));
  }
  const aliases = mods.flatMap((m) => m.funcs).filter((f) => f.alias && f.removed === undefined);
  out.push("## Old names (deprecated)", "");
  if (aliases.length) {
    out.push("Still available for old sources. Use the new name.", "", "| Old name | New name |", "|---|---|");
    for (const f of aliases) out.push(`| \`${f.alias}\` | \`${f.cname}\` |`);
  } else out.push("None.");
  out.push("", "## Removed", "");
  const removed = mods.flatMap((m) => m.funcs).filter((f) => f.removed !== undefined);
  if (removed.length) {
    out.push("| Name | Removed at level | Last signature |", "|---|---|---|");
    for (const f of removed) out.push(`| \`${f.cname}\` | ${f.removed} | \`${cell(ledgerSignature(f))}\` |`);
  } else out.push("None.");
  out.push(
    "",
    "## SDK helpers (not imports)",
    "",
    "`pal_strlen`, `pal_puts` and `pal_putu` are in `sdk/palanu.h` as `static inline` functions on top of `pal_write`.",
    "",
  );
  return out.join("\n");
}
