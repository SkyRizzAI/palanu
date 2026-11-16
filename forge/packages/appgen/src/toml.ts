// The TOML subset apps/apps.toml uses: [table] and [[array]] headers (dotted names allowed), and key = value lines where a
// value is a basic string, an integer, true or false, or an array of strings. Anything else is an error with a line number.
export type Value = string | number | boolean | string[];
export type Table = { [key: string]: Value | Table | Table[] };

function stripComment(line: string): string {
  let inStr = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c === '"' && line[i - 1] !== "\\") inStr = !inStr;
    else if (c === "#" && !inStr) return line.slice(0, i);
  }
  return line;
}

function parseValue(raw: string, where: string): Value {
  const v = raw.trim();
  if (v === "true") return true;
  if (v === "false") return false;
  if (/^-?\d+$/.test(v)) return Number(v);
  if (v.startsWith("[") && v.endsWith("]")) {
    const inner = v.slice(1, -1).trim();
    if (inner === "") return [];
    const items: string[] = [];
    for (const part of inner.split(",")) {
      const p = part.trim();
      if (p === "") continue; // trailing comma
      const s = parseValue(p, where);
      if (typeof s !== "string") throw new Error(`${where}: arrays hold strings only`);
      items.push(s);
    }
    return items;
  }
  if (v.startsWith('"') && v.endsWith('"') && v.length >= 2) return v.slice(1, -1).replace(/\\(.)/g, "$1");
  throw new Error(`${where}: unsupported value ${v}`);
}

function descend(root: Table, path: string[], where: string, arrayLast: boolean): Table {
  let cur: Table = root;
  for (let i = 0; i < path.length; i++) {
    const key = path[i];
    const last = i === path.length - 1;
    if (last && arrayLast) {
      const list = (cur[key] ??= [] as Table[]) as Table[];
      if (!Array.isArray(list)) throw new Error(`${where}: ${key} is not an array of tables`);
      const t: Table = {};
      list.push(t);
      return t;
    }
    const next = cur[key] ??= {} as Table;
    if (Array.isArray(next)) {
      cur = next[next.length - 1];
    } else if (typeof next === "object") {
      cur = next as Table;
    } else {
      throw new Error(`${where}: ${key} is already a value`);
    }
  }
  return cur;
}

export function parseToml(text: string): Table {
  const root: Table = {};
  let cur: Table = root;
  const lines = text.split("\n");
  for (let n = 0; n < lines.length; n++) {
    const where = `line ${n + 1}`;
    const line = stripComment(lines[n]).trim();
    if (line === "") continue;
    const arr = /^\[\[([A-Za-z0-9_.-]+)\]\]$/.exec(line);
    if (arr) {
      cur = descend(root, arr[1].split("."), where, true);
      continue;
    }
    const tab = /^\[([A-Za-z0-9_.-]+)\]$/.exec(line);
    if (tab) {
      cur = descend(root, tab[1].split("."), where, false);
      continue;
    }
    const kv = /^([A-Za-z0-9_-]+)\s*=\s*(.+)$/.exec(line);
    if (!kv) throw new Error(`${where}: cannot read '${line}'`);
    if (kv[1] in cur) throw new Error(`${where}: ${kv[1]} is set twice`);
    cur[kv[1]] = parseValue(kv[2], where);
  }
  return root;
}
