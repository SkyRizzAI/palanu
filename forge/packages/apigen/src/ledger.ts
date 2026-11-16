// api/symbols.csv: one row per public name, in definition order, aliases right after their function. It is generated;
// `./pal check` compares it with the committed copy (HEAD) so a released signature cannot change under the same name.
import { type ModuleDef, ledgerSignature } from "./model.ts";

export const LEDGER_HEADER = "name,since,signature,cap,wasm_name,status";

export interface LedgerRow {
  name: string;
  since: number;
  signature: string;
  cap: string;
  wasm_name: string;
  status: "stable" | "deprecated" | "removed";
}

export function ledgerRows(mods: ModuleDef[]): LedgerRow[] {
  const rows: LedgerRow[] = [];
  for (const m of mods) {
    for (const f of m.funcs) {
      const base = { since: f.since, signature: ledgerSignature(f), cap: f.cap ?? "", wasm_name: f.wasm };
      rows.push({ name: f.cname, ...base, status: f.removed !== undefined ? "removed" : "stable" });
      if (f.alias) rows.push({ name: f.alias, ...base, status: "deprecated" });
    }
  }
  return rows;
}

// Quotes a CSV field when it holds a comma or a quote (signatures do).
function field(v: string): string {
  return /[",\n]/.test(v) ? `"${v.replace(/"/g, '""')}"` : v;
}

export function renderLedger(rows: LedgerRow[]): string {
  const lines = [LEDGER_HEADER];
  for (const r of rows) lines.push([r.name, String(r.since), r.signature, r.cap, r.wasm_name, r.status].map(field).join(","));
  return lines.join("\n") + "\n";
}

// Splits one CSV line into fields, honouring double quotes.
function splitCsv(line: string): string[] {
  const out: string[] = [];
  let cur = "";
  let quoted = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (quoted) {
      if (c === '"' && line[i + 1] === '"') (cur += '"'), i++;
      else if (c === '"') quoted = false;
      else cur += c;
    } else if (c === '"') quoted = true;
    else if (c === ",") (out.push(cur), (cur = ""));
    else cur += c;
  }
  out.push(cur);
  return out;
}

export function parseLedger(text: string): { rows: LedgerRow[]; problems: string[] } {
  const problems: string[] = [];
  const rows: LedgerRow[] = [];
  const lines = text.split("\n").filter((l) => l.length > 0);
  if (lines[0] !== LEDGER_HEADER) problems.push(`api/symbols.csv: header must be "${LEDGER_HEADER}"`);
  lines.slice(1).forEach((line, i) => {
    const f = splitCsv(line);
    if (f.length !== 6) return void problems.push(`api/symbols.csv:${i + 2}: expected 6 fields, got ${f.length}`);
    rows.push({ name: f[0], since: Number(f[1]), signature: f[2], cap: f[3], wasm_name: f[4], status: f[5] as LedgerRow["status"] });
  });
  return { rows, problems };
}

/* The ledger rule: against the committed ledger, a released name keeps its signature, capability and import name, and
 * a row that disappears is an error unless it was already removed. Added rows are always allowed. */
export function compareLedger(headText: string, workText: string): string[] {
  if (!headText.trim()) return []; // no committed ledger yet: there is nothing to keep stable
  const head = parseLedger(headText);
  const work = parseLedger(workText);
  const errors = [...work.problems];
  const now = new Map(work.rows.map((r) => [r.name, r]));
  for (const h of head.rows) {
    if (h.status === "removed") continue;
    const w = now.get(h.name);
    if (!w) {
      errors.push(`api: ${h.name} removed (keep its row and set status=removed)`);
      continue;
    }
    if (w.status === "removed") continue;
    if (w.signature !== h.signature) errors.push(`api: ${h.name} changed signature (use a new name)`);
    else if (w.cap !== h.cap) errors.push(`api: ${h.name} changed cap (use a new name)`);
    else if (w.wasm_name !== h.wasm_name) errors.push(`api: ${h.name} changed wasm_name (use a new name)`);
  }
  return errors;
}
