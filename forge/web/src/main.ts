// Palanu Forge in the browser: one page for a PalanuOS device, whichever it is. The device is the simulator (PalanuOS built to
// WebAssembly, running in this page) or a remote board (USB, Bluetooth, Wi-Fi/LAN). Both speak Palanu Link, so the Display,
// Files, System and Store sections are the same code for both. Everything runs in this page; a pairing token is kept in this
// browser's localStorage only.
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { Link, LinkError, LINK_ERR, type DeviceInfo, type Entry, type Transport } from "./link.ts";
import { connectBle, connectUsb, connectWs, support, WS_PORT } from "./transports.ts";
import * as sim from "./sim/sim.ts";
import { sha256Hex } from "./crypto.ts";

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const el = <T extends HTMLElement>(tag: string, cls?: string, text?: string): T =>
  Object.assign(document.createElement(tag), { className: cls ?? "", textContent: text ?? "" }) as T;
const store = {
  get: (k: string) => { try { return localStorage.getItem(k); } catch { return null; } },
  set: (k: string, v: string) => { try { localStorage.setItem(k, v); } catch { /* private window */ } },
};
const LINKS = "palanu.webforge.links", ADDR = "palanu.webforge.addr", THEME = "palanu.forge.theme", CATALOG = "palanu.forge.catalog";
const SECTION = "palanu.forge.section";

type Pairing = { clientId: string; token: string; name: string; paired: string };
const loadAll = (): Record<string, Pairing> => { try { return JSON.parse(store.get(LINKS) || "{}"); } catch { return {}; } };
const saveAll = (m: Record<string, Pairing>) => store.set(LINKS, JSON.stringify(m));

type Source = "none" | "sim" | "remote";
let source: Source = "none";
let link: Link | null = null;
let info: DeviceInfo = {};
let since = 0;

// ---------------------------------------------------------------- small dialogs (confirm, prompt) in the page's look

function ask(title: string, text: string, input?: string): Promise<string | null> {
  return new Promise((resolve) => {
    const dlg = $<HTMLDialogElement>("ask"), field = $<HTMLInputElement>("ask-input");
    $("ask-title").textContent = title;
    $("ask-text").textContent = text;
    field.hidden = input === undefined;
    field.value = input ?? "";
    const done = (v: string | null) => { dlg.close(); $("ask-form").onsubmit = null; $("ask-no").onclick = null; resolve(v); };
    $("ask-form").onsubmit = (e) => { e.preventDefault(); done(input === undefined ? "yes" : field.value); };
    $("ask-no").onclick = () => done(null);
    dlg.oncancel = () => done(null);
    dlg.showModal();
    if (input !== undefined) { field.focus(); field.select(); }
  });
}

// ---------------------------------------------------------------- state: the chip, the footer, the banner

type Kind = "idle" | "busy" | "ready" | "error";
function setState(kind: Kind, text: string) {
  document.body.dataset.state = kind;
  $("f-state").textContent = text.toUpperCase();
  if (kind !== "ready") $("chip-name").textContent = kind === "idle" ? "No device" : text;
}
function banner(text: string) { $("banner").textContent = text; }

function setSource(s: Source) {
  source = s;
  document.body.dataset.source = s;
  $("act-restart").hidden = s === "none";
  $("act-stop").hidden = s === "none";
  $("act-stop").textContent = s === "remote" ? "DISCONNECT" : "SHUTDOWN";
  $("act-restart").title = s === "remote" ? "Restart the board (reboot)" : "Restart the system (its files stay)";
  if (s === "none") { $("chip-kind").textContent = ""; $("f-plp").textContent = "PLP: —"; $("f-src").textContent = ""; $("f-fw").textContent = ""; }
  sim.applyScreen();
  refreshFooter();
  if (s === "remote" && section === "setup") showSection("display", false);
}

function refreshFooter() {
  const kind = link?.t.kind ?? "";
  $("f-plp").textContent = `PLP: ${kind ? kind.toUpperCase().replace(/[\s/]+/g, "-") : "—"}`;
  $("f-src").textContent = source === "sim" ? "simulator" : source === "remote" ? String(info.board ?? "board") : "";
  $("f-fw").textContent = info.ver ? `fw: ${info.ver}` : "";
  const c = $<HTMLCanvasElement>("screen");
  $("f-res").textContent = source === "none" ? "" : `${c.width}×${c.height}`;
  $("res-label").textContent = `${c.width}×${c.height}`;
  $("board-label").textContent = source === "sim" ? "PALANU · SIMULATOR" : source === "remote" ? `PALANU · ${String(info.board ?? "BOARD").toUpperCase()}` : "PALANU";
  st.target();
}

setInterval(() => {
  if (!since) { $("f-up").textContent = ""; return; }
  const s = Math.floor((performance.now() - since) / 1000);
  $("f-up").textContent = [Math.floor(s / 3600), Math.floor(s / 60) % 60, s % 60].map((v) => String(v).padStart(2, "0")).join(":");
}, 1000);

// ---------------------------------------------------------------- theme

function applyTheme(t: string | null) {
  if (t === "dark" || t === "light") document.documentElement.dataset.theme = t;
  else delete document.documentElement.dataset.theme;
  const dark = t ? t === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
  $("theme").textContent = dark ? "DARK" : "LIGHT";
  $("theme").title = dark ? "Switch to the light look" : "Switch to the dark look";
  if (term) term.options.theme = termTheme();
}

// ---------------------------------------------------------------- sections

const SECTIONS = ["display", "files", "system", "setup", "store"];
let section = "display";

function showSection(name: string, remember = true) {
  if (!SECTIONS.includes(name)) name = "display";
  section = name;
  document.body.dataset.section = name;
  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-section]")) b.setAttribute("aria-selected", String(b.dataset.section === name));
  for (const m of document.querySelectorAll<HTMLElement>("[data-sec]")) m.hidden = m.dataset.sec !== name;
  for (const a of document.querySelectorAll<HTMLAnchorElement>("[data-work]")) {
    const on = (a.dataset.work === "store") === (name === "store");
    if (on) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current");
  }
  const hash = name === "display" ? "" : name === "setup" ? "#sim" : `#${name}`;
  if (location.hash !== hash) history.replaceState(null, "", hash || location.pathname);
  if (remember) store.set(SECTION, name);
  void link?.subscribeScreen(source === "remote" && name === "display").catch(() => {});
  if (name === "display") requestAnimationFrame(redrawTerm);
  if (name === "files" && link) void listDir(cwd);
  if (name === "store") void st.ensure();
}

function sectionFromHash(): string {
  const h = location.hash.slice(1);
  return h === "sim" ? "setup" : SECTIONS.includes(h) ? h : "display";
}

// ---------------------------------------------------------------- the console: terminal and logs

let term: Terminal | null = null;
let fit: FitAddon | null = null;
let shellOpen = false;
let ctab = "term";

function termTheme() {
  const css = getComputedStyle(document.documentElement);
  const v = (n: string) => css.getPropertyValue(n).trim();
  return { background: v("--term-bg"), foreground: v("--term-fg"), cursor: v("--accent"), selectionBackground: "#4a4366" };
}

function ensureTerm() {
  if (term) return;
  term = new Terminal({ fontFamily: "ui-monospace, 'SF Mono', SFMono-Regular, Menlo, Consolas, monospace", fontSize: 13, cursorBlink: true, theme: termTheme() });
  fit = new FitAddon();
  term.loadAddon(fit);
  term.open($("term"));
  term.onData((d) => link?.ttyWrite(new TextEncoder().encode(d)));
  term.onResize(({ cols, rows }) => { if (shellOpen) void link?.resize(cols, rows); });
  new ResizeObserver(() => { if (section === "display" && ctab === "term") fit?.fit(); }).observe($("term"));
}

/* xterm does not draw while its box is hidden (display: none): fit it and paint every row when the box shows again */
function redrawTerm() {
  if (!term) return;
  fit?.fit();
  term.refresh(0, term.rows - 1);
}

function openShell() {
  ensureTerm();
  fit?.fit();
  if (!link || shellOpen) return;
  link.onTty = (d) => term!.write(d);
  link.onTtyClose = () => { shellOpen = false; term!.write("\r\n\x1b[2m[shell ended — press Enter for another]\x1b[0m\r\n"); };
  void link.openShell(term!.cols, term!.rows);
  shellOpen = true;
}

function showCtab(name: string) {
  ctab = name;
  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-ctab]")) b.setAttribute("aria-selected", String(b.dataset.ctab === name));
  for (const p of document.querySelectorAll<HTMLElement>("[data-cpanel]")) p.hidden = p.dataset.cpanel !== name;
  if (name === "term") requestAnimationFrame(() => { redrawTerm(); term?.focus(); });
  else dirtyLog = true;
}

interface Line { t: string; err: boolean; ts: number }
const lines: Line[] = [];
let paused = false, dirtyLog = true, partial = "";
const PROBLEM = /\b(error|fail(ed|ure)?|panic|assert|abort|refused|denied|cannot|overflow)\b|\bE \(/i;

function addLine(t: string, err: boolean) {
  lines.push({ t, err: err || PROBLEM.test(t), ts: since ? performance.now() - since : 0 });
  if (lines.length > 5000) lines.splice(0, lines.length - 5000);
  if (!paused) dirtyLog = true;
}
/* a board's console bytes (outside Palanu Link frames), cut into lines */
function addConsole(d: Uint8Array) {
  const text = partial + new TextDecoder().decode(d).replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "").replace(/\r/g, "");
  const parts = text.split("\n");
  partial = parts.pop() ?? "";
  for (const p of parts) if (p.trim()) addLine(p, false);
}
function renderLog() {
  if (!dirtyLog || ctab !== "logs" || section !== "display") return;
  dirtyLog = false;
  const log = $("log");
  const f = $<HTMLInputElement>("l-filter").value.toLowerCase(), only = $<HTMLInputElement>("l-err").checked;
  const stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
  const shown = lines.filter((l) => (!only || l.err) && (!f || l.t.toLowerCase().includes(f)));
  const frag = document.createDocumentFragment();
  for (const l of shown.slice(-1500)) frag.append(el("div", "ln" + (l.err ? " bad" : ""), `${(l.ts / 1000).toFixed(2).padStart(7)}  ${l.t}`));
  log.replaceChildren(frag);
  $("l-count").textContent = `${shown.length} of ${lines.length} lines`;
  if (stick) log.scrollTop = log.scrollHeight;
}
(function logLoop() { renderLog(); requestAnimationFrame(logLoop); })();

// ---------------------------------------------------------------- the screen and the keys

let img: ImageData | null = null;
function paint(d: { x: number; y: number; w: number; h: number }) {
  if (!link || source !== "remote") return;
  const s = link.screen, c = $<HTMLCanvasElement>("screen");
  if (c.width !== s.w || c.height !== s.h) { c.width = s.w; c.height = s.h; c.style.aspectRatio = `${s.w} / ${s.h}`; img = null; refreshFooter(); }
  const ctx = c.getContext("2d")!;
  if (!img) img = ctx.createImageData(s.w, s.h);
  const px = img.data;
  for (let y = d.y; y < d.y + d.h && y < s.h; y++)
    for (let x = d.x; x < d.x + d.w && x < s.w; x++) {
      const v = s.is565 ? s.rgb[y * s.w + x] : s.pal[s.px[y * s.w + x]], o = (y * s.w + x) * 4;
      px[o] = ((v >> 11) & 31) * 255 / 31; px[o + 1] = ((v >> 5) & 63) * 255 / 63; px[o + 2] = (v & 31) * 255 / 31; px[o + 3] = 255;
    }
  ctx.putImageData(img, 0, 0, d.x, d.y, d.w, d.h);
}

// pal_keysym_t (system/include/pal/input.h)
const KEY: Record<string, number> = {
  Enter: 2, Escape: 3, Backspace: 4, Tab: 5, Delete: 6, ArrowUp: 7, ArrowDown: 8, ArrowLeft: 9, ArrowRight: 10,
  Home: 11, End: 12, PageUp: 13, PageDown: 14, Insert: 18, F1: 15, F2: 16,
};
const NAMED_SYM: Record<string, number> = { enter: 2, esc: 3, up: 7, down: 8, left: 9, right: 10, switch: 15, "home-app": 16, vkb: 17 };

function press(name: string) {
  if (source === "sim") sim.simKey(name);
  else if (source === "remote" && NAMED_SYM[name] !== undefined) void link?.key(NAMED_SYM[name]);
}

function onRemoteKey(e: KeyboardEvent) {
  if (!link || source !== "remote" || e.metaKey) return;
  const mods = (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0);
  if (KEY[e.key] !== undefined) { e.preventDefault(); void link.key(KEY[e.key], 0, mods); return; }
  if ([...e.key].length === 1) { e.preventDefault(); void link.key(1, e.key.codePointAt(0)!, mods & ~1); }
}

function screenshot() {
  $<HTMLCanvasElement>("screen").toBlob((b) => {
    if (!b) return;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(b);
    a.download = `palanu-${new Date().toISOString().replace(/[:.]/g, "-")}.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  });
}

// ---------------------------------------------------------------- one device: say hello, trust, ready

async function attach(t: Transport, s: Source) {
  link = new Link(t);
  link.onConsole = addConsole;
  link.onScreen = paint;
  t.onClose(() => { if (link?.t === t) dropped(); });
  setState("busy", s === "sim" ? "Starting" : `Hello over ${t.kind}`);
  info = await link.hello(s === "sim" ? 60 : 15);
  if (!info.trusted) await trust();
  info = await link.hello();
  since = since || performance.now();
  setSource(s);
  setState("ready", "Ready");
  $("chip-name").textContent = String(info.name ?? (s === "sim" ? "simulator" : "PalanuOS"));
  $("chip-kind").textContent = s === "sim" ? "SIM" : t.kind === "Wi-Fi/LAN" ? "WI-FI" : t.kind.toUpperCase();
  renderInfo();
  refreshFooter();
  shellOpen = false;
  term?.reset();
  openShell();
  showSection(section, false);
  if (section === "store") void st.load();
}

async function connectRemote(make: () => Promise<Transport>) {
  $<HTMLDialogElement>("connect").close();
  if (source === "sim") { banner("The simulator is running in this page: shut it down first, then connect a board."); return; }
  try {
    setState("busy", "Connecting");
    banner("");
    const t = await make();
    await attach(t, "remote");
  } catch (e) {
    const msg = (e as Error).message || String(e);
    if (/No port selected|User cancelled|cancelled the requestDevice|pairing cancelled/i.test(msg)) setState("idle", "Idle");
    else { setState("error", "Error"); banner(msg); }
    if (link) { const l = link; link = null; await l.close().catch(() => {}); }
    setSource("none");
  }
}

/** Makes the link trusted: the stored token for this device, or a pairing with the device's code. */
async function trust() {
  const id = String(info.id ?? "");
  const p = loadAll()[id];
  if (p) {
    try {
      setState("busy", "Signing in");
      await link!.auth(p.clientId, p.token);
      return;
    } catch (e) {
      if (e instanceof LinkError && e.code === LINK_ERR.busy) throw new Error("too many failed sign-ins: wait 30 seconds");
      banner("The saved pairing was refused (revoked on the device?): pair again.");
    }
  }
  const pairing = await askPairing();
  const all = loadAll();
  all[id] = { ...pairing, name: String(info.name ?? id), paired: new Date().toISOString() };
  saveAll(all);
  await link!.auth(pairing.clientId, pairing.token);
}

/** A name the device can tell apart in its list: "Chrome on macOS" rather than "Web Forge" for every browser. */
function browserName(): string {
  const ua = navigator.userAgent;
  const browser = /Edg\//.test(ua) ? "Edge" : /OPR\//.test(ua) ? "Opera" : /Chrome\//.test(ua) ? "Chrome" : /Firefox\//.test(ua) ? "Firefox" : /Safari\//.test(ua) ? "Safari" : "Browser";
  const os = /Android/.test(ua) ? "Android" : /iPhone|iPad/.test(ua) ? "iOS" : /Mac OS X/.test(ua) ? "macOS" : /Windows/.test(ua) ? "Windows" : /Linux/.test(ua) ? "Linux" : "";
  return (os ? `${browser} on ${os}` : browser).slice(0, 24);
}

function askPairing(): Promise<{ clientId: string; token: string }> {
  return new Promise((resolve, reject) => {
    const dlg = $<HTMLDialogElement>("pair");
    $("pair-device").textContent = String(info.name ?? "PalanuOS").toUpperCase();
    $<HTMLInputElement>("pair-code").value = "";
    const nameField = $<HTMLInputElement>("pair-name");
    if (!nameField.dataset.edited) nameField.value = browserName();
    nameField.oninput = () => (nameField.dataset.edited = "1");
    $("pair-err").textContent = "";
    setState("busy", "Pairing");
    dlg.showModal();
    const form = $<HTMLFormElement>("pair-form");
    const cancel = () => { dlg.close(); cleanup(); reject(new Error("pairing cancelled")); };
    const submit = async (ev: SubmitEvent) => {
      ev.preventDefault();
      const code = $<HTMLInputElement>("pair-code").value.trim();
      const name = (nameField.value.trim() || "Web Forge").slice(0, 24);
      try {
        $("pair-err").textContent = "Pairing…";
        const r = await link!.pair(code, name);
        dlg.close(); cleanup(); resolve(r);
      } catch (e) {
        const le = e as LinkError;
        $("pair-err").textContent = le.code === LINK_ERR.pairClosed ? "No pairing window is open on the device: open it again" : le.message;
      }
    };
    const cleanup = () => { form.removeEventListener("submit", submit); $("pair-cancel").removeEventListener("click", cancel); dlg.oncancel = null; };
    form.addEventListener("submit", submit);
    $("pair-cancel").addEventListener("click", cancel);
    dlg.oncancel = (e) => { e.preventDefault(); cancel(); };
  });
}

function dropped() {
  link = null;
  shellOpen = false;
  term?.write("\r\n\x1b[2m[disconnected]\x1b[0m\r\n");
  if (source === "remote") { since = 0; setSource("none"); setState("error", "Disconnected"); }
}

async function disconnect() {
  const l = link;
  link = null;
  if (l) { try { await l.subscribeScreen(false); await l.closeShell(); } catch { /* gone */ } await l.close().catch(() => {}); }
  shellOpen = false;
  since = 0;
  info = {};
  setSource("none");
  setState("idle", "Idle");
}

// ---------------------------------------------------------------- the simulator as the device

async function startSimulator() {
  const why = sim.simBlocker();
  if (why) { $("sim-note").textContent = why; return; }
  if (source === "remote") await disconnect();
  try {
    banner("");
    setState("busy", "Loading");
    const e = await sim.startSim();
    if (!e) return; /* the page reloads with isolation on */
    setSource("sim");
    since = performance.now();
    setState("busy", "Booting");
    $<HTMLCanvasElement>("screen").focus();
    await attach(sim.simTransport(e), "sim");
  } catch (err) {
    setState("error", "Error");
    banner((err as Error).message || String(err));
  }
}

// ---------------------------------------------------------------- files

let cwd = "/home";
const fmtSize = (n: number) => n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KB` : `${(n / 1048576).toFixed(1)} MB`;
const join = (a: string, b: string) => (a.endsWith("/") ? a : a + "/") + b;
const TEXT_RE = /\.(txt|md|sh|conf|cfg|ini|json|toml|csv|log|c|h|js|ts|py|lua)$|^[^.]+$/i;
const note = (t: string) => ($("files-note").textContent = t);

async function listDir(path: string) {
  if (!link) return;
  const tbody = $("files-body");
  $("files-path").textContent = path;
  tbody.innerHTML = `<tr><td colspan="3" class="muted">Loading…</td></tr>`;
  try {
    const items: Entry[] = path === "/" ? ["home", "data", "apps", "tmp", "sd", "etc", "bin"].map((n) => ({ name: n, dir: true, size: 0 })) : await link.list(path);
    cwd = path;
    items.sort((a, b) => Number(b.dir) - Number(a.dir) || a.name.localeCompare(b.name));
    tbody.innerHTML = "";
    if (path !== "/") items.unshift({ name: "..", dir: true, size: 0 });
    for (const it of items) {
      const tr = el("tr");
      const name = el("td");
      const btn = el<HTMLButtonElement>("button", "linkbtn" + (it.dir ? " dir" : ""), (it.dir ? "▸ " : "  ") + it.name + (it.dir && it.name !== ".." ? "/" : ""));
      btn.type = "button";
      btn.onclick = () => {
        if (it.name === "..") void listDir(path.replace(/\/[^/]+\/?$/, "") || "/");
        else if (it.dir) void listDir(join(path, it.name));
        else void download(join(path, it.name), it.name);
      };
      name.append(btn);
      const size = el("td", "num", it.dir ? "" : fmtSize(it.size));
      const act = el("td", "acts");
      const full = join(path, it.name);
      const button = (label: string, fn: () => void, cls = "") => { const b = el<HTMLButtonElement>("button", `btn px ${cls}`, label); b.type = "button"; b.onclick = fn; act.append(b); };
      if (!it.dir) button("GET", () => void download(full, it.name));
      if (!it.dir && TEXT_RE.test(it.name) && it.size <= 262144) button("EDIT", () => void editFile(full));
      if (!it.dir && it.name.endsWith(".papp")) button("INSTALL", () => void installAt(full), "solid");
      if (it.name !== ".." && path !== "/") {
        button("RENAME", () => void renameEntry(path, it.name));
        button("DEL", () => void removeEntry(full, it.dir), "rose");
      }
      tr.append(name, size, act);
      tbody.append(tr);
    }
    if (items.length <= 1 && path !== "/") tbody.insertAdjacentHTML("beforeend", `<tr><td colspan="3" class="muted">Empty folder</td></tr>`);
  } catch (e) {
    tbody.innerHTML = "";
    const tr = el("tr"), td = el("td", "err", (e as Error).message); td.colSpan = 3; tr.append(td); tbody.append(tr);
  }
}

async function removeEntry(path: string, dir: boolean) {
  if (!link || !(await ask("DELETE", `Delete ${path}?${dir ? " A folder must be empty." : ""}`))) return;
  try { await link.remove(path); note(`Deleted ${path}`); void listDir(cwd); } catch (e) { note((e as Error).message); }
}

async function renameEntry(dir: string, name: string) {
  const to = link ? await ask("RENAME", `New name for ${name}`, name) : null;
  if (!link || !to || to === name || to.includes("/")) return;
  try { await link.rename(join(dir, name), join(dir, to)); void listDir(cwd); } catch (e) { note((e as Error).message); }
}

async function makeDir() {
  const name = link && cwd !== "/" ? await ask("NEW FOLDER", `A folder in ${cwd}`, "") : null;
  if (!link || !name || name.includes("/")) return;
  try { await link.mkdir(join(cwd, name)); void listDir(cwd); } catch (e) { note((e as Error).message); }
}

async function installAt(path: string) {
  if (!link) return;
  note(`Installing ${path}…`);
  try { await link.install(path); note(`Installed ${path}: it is in the device's launcher and runs by its id`); }
  catch (e) { note(`Not installed: ${(e as Error).message}`); }
}

/* A text file in a dialog: read, edit, write back whole. */
async function editFile(path: string) {
  if (!link) return;
  const dlg = $<HTMLDialogElement>("edit"), text = $<HTMLTextAreaElement>("edit-text");
  $("edit-path").textContent = path;
  $("edit-err").textContent = "";
  try { text.value = new TextDecoder().decode(await link.read(path)); } catch (e) { note((e as Error).message); return; }
  dlg.showModal();
  const form = $<HTMLFormElement>("edit-form");
  const done = () => { form.onsubmit = null; $("edit-cancel").onclick = null; };
  $("edit-cancel").onclick = () => { done(); dlg.close(); };
  form.onsubmit = async (ev) => {
    ev.preventDefault();
    try {
      await link!.write(path, new TextEncoder().encode(text.value));
      done(); dlg.close(); note(`Saved ${path}`); void listDir(cwd);
    } catch (e) { $("edit-err").textContent = (e as Error).message; }
  };
}

async function download(path: string, name: string) {
  if (!link) return;
  try {
    note(`Reading ${path}…`);
    const data = await link.read(path, (n) => note(`Reading ${path}: ${fmtSize(n)}`));
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([data as BlobPart]));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    note(`Saved ${name} (${fmtSize(data.length)})`);
  } catch (e) { note((e as Error).message); }
}

async function upload(files: FileList) {
  if (!link) return;
  for (const f of Array.from(files)) {
    const dest = join(cwd, f.name);
    try {
      const data = new Uint8Array(await f.arrayBuffer());
      await link.write(dest, data, (n) => note(`Sending ${f.name}: ${Math.round((n / data.length) * 100)}%`));
      note(`Sent ${f.name} to ${dest}`);
    } catch (e) { note(`${f.name}: ${(e as Error).message}`); }
  }
  void listDir(cwd);
}

// ---------------------------------------------------------------- system

function renderInfo() {
  const dl = $("info");
  dl.replaceChildren();
  const labels: Record<string, string> = { os: "System", ver: "Version", board: "Board", chip: "Chip", name: "Name", id: "Device id",
    uptime: "Uptime (s)", mem_free: "Internal RAM free", psram_free: "PSRAM free", slot: "Firmware slot", build: "Build", ota: "Updates" };
  for (const [k, v] of Object.entries(info)) {
    if (k === "trusted" || v === "" || v === null) continue;
    dl.append(el("dt", "", labels[k] ?? k), el("dd", "", typeof v === "number" && /free/.test(k) ? fmtSize(v) : String(v)));
  }
  const p = loadAll()[String(info.id ?? "")];
  $("paired-note").textContent = p ? `Paired as “${p.name}” on ${new Date(p.paired).toLocaleString()}.` :
    link?.t.kind === "USB" ? "Trusted over the cable: no pairing needed." : "";
}

async function flash(file: File) {
  if (!link) return;
  if (!$<HTMLInputElement>("ota-confirm").checked) { $("ota-note").textContent = "Tick the box first: the device restarts into the new firmware."; return; }
  try {
    const image = new Uint8Array(await file.arrayBuffer());
    const bar = $<HTMLProgressElement>("ota-bar");
    bar.hidden = false;
    await link.ota(image, (d, t) => { bar.value = d / t; $("ota-note").textContent = `Writing ${fmtSize(d)} of ${fmtSize(t)}`; });
    $("ota-note").textContent = "Done: the device is restarting into the new firmware.";
  } catch (e) { $("ota-note").textContent = (e as Error).message; }
}

/** Restart: the simulator reloads with its files; a board runs `reboot` in a shell. */
async function restart() {
  if (source === "sim") { sim.restartSim(); return; }
  if (!link) return;
  if (!(await ask("RESTART", `Restart ${info.name ?? "the board"}? The link drops and comes back after the boot.`))) return;
  openShell();
  await link.ttyWrite(new TextEncoder().encode("\x15reboot\r"));
}

// ---------------------------------------------------------------- store: a catalogue in the browser, installed over the link

interface App { id: string; version: string; name: string; size: number; sha: string; url: string; summary: string }
const DEFAULT_CATALOG = "https://raw.githubusercontent.com/SkyRizzAI/palanu/main/store/index.txt";

const st = {
  apps: [] as App[],
  installed: new Set<string>(),
  loaded: "",
  target() {
    $("st-target").textContent = source === "none" ? "NO DEVICE" : String(info.name ?? (source === "sim" ? "SIMULATOR" : "BOARD")).toUpperCase();
  },
  async ensure() {
    const url = $<HTMLInputElement>("st-url").value.trim();
    if (this.loaded !== url) await this.load();
  },
  async load() {
    const url = $<HTMLInputElement>("st-url").value.trim();
    const out = $("st-note");
    if (!url) return;
    out.textContent = "Reading the catalogue…";
    try {
      const r = await fetch(url, { cache: "no-store" });
      if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
      const text = await r.text();
      if (!text.startsWith("palanu-store")) throw new Error("not a catalogue (the first line is not palanu-store)");
      this.apps = [];
      for (const ln of text.split("\n").slice(1)) {
        const f = ln.replace(/\r$/, "").split("\t");
        if (f.length < 6 || !f[0] || f[0].startsWith("firmware:")) continue;
        this.apps.push({ id: f[0], version: f[1], name: f[2], size: Number(f[3]) || 0, sha: f[4].toLowerCase(), url: new URL(f[5], new URL(url, location.href)).href, summary: f[6] ?? "" });
      }
      this.loaded = url;
      store.set(CATALOG, url);
      out.textContent = `${this.apps.length} apps in the catalogue.`;
    } catch (e) {
      this.apps = [];
      out.textContent = `Cannot read the catalogue: ${(e as Error).message}. A catalogue on another site must allow this page to read it (CORS).`;
    }
    await this.scan();
    this.render();
  },
  async scan() {
    this.installed.clear();
    if (!link) return;
    try { for (const e of await link.list("/apps")) if (!e.name.startsWith(".")) this.installed.add(e.name.replace(/\.papp$/, "")); } catch { /* no /apps yet */ }
  },
  render() {
    const q = $<HTMLInputElement>("st-q").value.trim().toLowerCase();
    const list = this.apps.filter((a) => !q || `${a.id} ${a.name} ${a.summary}`.toLowerCase().includes(q));
    $("st-list").replaceChildren(...list.map((a) => {
      const li = el("li", "app");
      const tx = el("div", "tx");
      tx.append(el("div", "nm", a.name || a.id), el("div", "ds", a.summary || a.id), el("div", "vr", `${a.id} · v${a.version} · ${fmtSize(a.size)}`));
      const have = this.installed.has(a.id);
      const b = el<HTMLButtonElement>("button", `btn small px ${have ? "" : "solid"}`, have ? "REINSTALL" : "INSTALL");
      b.type = "button";
      b.disabled = !link;
      b.title = link ? "" : "Start the simulator or connect a board first";
      b.onclick = () => void this.install(a, b);
      li.append(el("div", "ic", (a.name || a.id).slice(0, 1).toUpperCase()), tx, b);
      return li;
    }));
    if (!list.length && this.apps.length) $("st-list").append(el("li", "muted", "Nothing matches."));
  },
  async install(a: App, b: HTMLButtonElement) {
    if (!link) return;
    const out = $("st-note");
    b.disabled = true;
    try {
      out.textContent = `Downloading ${a.name}…`;
      const r = await fetch(a.url, { cache: "no-store" });
      if (!r.ok) throw new Error(`download: ${r.status}`);
      const data = new Uint8Array(await r.arrayBuffer());
      if (a.sha && (await sha256Hex(data)) !== a.sha) throw new Error("the download does not match the catalogue's checksum");
      const tmp = `/tmp/${a.id}.papp`;
      await link.write(tmp, data, (n) => (out.textContent = `Sending ${a.name}: ${Math.round((n / data.length) * 100)}%`));
      out.textContent = `Installing ${a.name}…`;
      await link.install(tmp);
      await link.remove(tmp).catch(() => {});
      out.textContent = `Installed ${a.name} on ${info.name ?? "the device"}: it is in the launcher.`;
      await this.scan();
      this.render();
    } catch (e) {
      out.textContent = `${a.name}: ${(e as Error).message}`;
    }
    b.disabled = false;
  },
};

async function defaultCatalog(): Promise<string> {
  const saved = store.get(CATALOG);
  if (saved) return saved;
  if (location.protocol.startsWith("http")) {
    try { const r = await fetch("store/index.txt", { method: "HEAD", cache: "no-store" }); if (r.ok) return new URL("store/index.txt", location.href).href; } catch { /* not served here */ }
  }
  return DEFAULT_CATALOG;
}

// ---------------------------------------------------------------- wiring

async function init() {
  applyTheme(store.get(THEME));
  matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => applyTheme(store.get(THEME)));
  $("theme").onclick = () => {
    const dark = document.documentElement.dataset.theme ? document.documentElement.dataset.theme === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
    store.set(THEME, dark ? "light" : "dark");
    applyTheme(dark ? "light" : "dark");
  };

  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-section]")) b.onclick = () => showSection(b.dataset.section!);
  for (const a of document.querySelectorAll<HTMLAnchorElement>("[data-work]")) a.onclick = (e) => { e.preventDefault(); showSection(a.dataset.work === "store" ? "store" : (store.get(SECTION) && store.get(SECTION) !== "store" ? store.get(SECTION)! : "display")); };
  addEventListener("hashchange", () => showSection(sectionFromHash()));
  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-ctab]")) b.onclick = () => showCtab(b.dataset.ctab!);
  const cats = [...document.querySelectorAll<HTMLButtonElement>("[data-cat]")];
  const showCat = (name: string) => {
    for (const c of cats) c.setAttribute("aria-selected", String(c.dataset.cat === name));
    for (const p of document.querySelectorAll<HTMLElement>("[data-catp]")) p.hidden = p.dataset.catp !== name;
    store.set("palanu.forge.cat", name);
  };
  for (const c of cats) c.onclick = () => showCat(c.dataset.cat!);
  showCat(store.get("palanu.forge.cat") || "screen");

  /* the screen, the pad */
  const canvas = $<HTMLCanvasElement>("screen");
  canvas.addEventListener("keydown", onRemoteKey);
  canvas.addEventListener("click", () => canvas.focus());
  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-key]")) b.onclick = () => { press(b.dataset.key!); canvas.focus(); };
  $("kb-toggle").onclick = () => { const kb = $("kb"); kb.hidden = !kb.hidden; $("kb-toggle").setAttribute("aria-pressed", String(!kb.hidden)); };
  $("shot").onclick = screenshot;
  $("start-sim").onclick = () => void startSimulator();
  $("open-connect").onclick = () => $<HTMLDialogElement>("connect").showModal();
  $("chip").onclick = () => { if (source === "none") $<HTMLDialogElement>("connect").showModal(); else showSection("system"); };
  $("act-restart").onclick = () => void restart();
  $("act-stop").onclick = async () => {
    if (source === "sim") { if (await ask("SHUT DOWN", "Stop the simulator? Its files are kept in this browser.")) sim.stopSim(); }
    else void disconnect();
  };
  const blocker = sim.simBlocker();
  if (blocker) { $<HTMLButtonElement>("start-sim").disabled = true; $("sim-note").textContent = blocker; }
  else $("sim-note").textContent = "Sim firmware: this build (./pal web).";

  /* the connect dialog */
  const addr = $<HTMLInputElement>("ws-addr");
  addr.value = store.get(ADDR) || "";
  addr.placeholder = `192.168.1.3 (port ${WS_PORT})`;
  $<HTMLButtonElement>("c-usb").disabled = !support.usb;
  $<HTMLButtonElement>("c-ble").disabled = !support.ble;
  if (!support.usb) $("c-usb").title = "Web Serial is in Chrome and Edge on a computer";
  if (!support.ble) $("c-ble").title = "Web Bluetooth is in Chrome and Edge";
  if (support.wsBlocked) $("ws-note").textContent = "This page is on https: browsers block ws:// to the device. Open Palanu Forge from the file or http://localhost.";
  $("c-usb").onclick = () => void connectRemote(connectUsb);
  $("c-ble").onclick = () => void connectRemote(connectBle);
  const goWs = () => {
    if (!addr.value.trim()) { addr.focus(); return; }
    store.set(ADDR, addr.value.trim());
    void connectRemote(() => connectWs(addr.value));
  };
  $("c-ws").onclick = goWs;
  addr.onkeydown = (e) => { if (e.key === "Enter") { e.preventDefault(); goWs(); } };
  $("connect-cancel").onclick = () => $<HTMLDialogElement>("connect").close();

  /* the console */
  $("l-filter").oninput = () => (dirtyLog = true);
  $("l-err").onchange = () => (dirtyLog = true);
  const wrap = store.get("palanu.forge.wrap") !== "0";
  $<HTMLInputElement>("l-wrap").checked = wrap;
  $("log").dataset.wrap = wrap ? "1" : "0";
  $("l-wrap").onchange = () => { const on = $<HTMLInputElement>("l-wrap").checked; $("log").dataset.wrap = on ? "1" : "0"; store.set("palanu.forge.wrap", on ? "1" : "0"); dirtyLog = true; };
  $("l-pause").onclick = () => { paused = !paused; $("l-pause").setAttribute("aria-pressed", String(paused)); $("l-pause").textContent = paused ? "RESUME" : "PAUSE"; dirtyLog = !paused; };
  $("l-clear").onclick = () => { lines.length = 0; dirtyLog = true; };
  $("l-save").onclick = () => {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([lines.map((l) => `${(l.ts / 1000).toFixed(2)} ${l.t}`).join("\n") + "\n"], { type: "text/plain" }));
    a.download = "palanu.log"; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  };
  ensureTerm();
  term!.onKey(({ domEvent }) => { if (!shellOpen && link && domEvent.key === "Enter") openShell(); });

  /* files, system */
  $("files-up").onclick = () => $<HTMLInputElement>("files-input").click();
  $<HTMLInputElement>("files-input").onchange = (e) => { const f = (e.target as HTMLInputElement).files; if (f?.length) void upload(f); (e.target as HTMLInputElement).value = ""; };
  $("files-refresh").onclick = () => void listDir(cwd);
  $("files-mkdir").onclick = () => void makeDir();
  $("files-root").onclick = () => void listDir("/");
  $("ping").onclick = async () => { try { $("ping-out").textContent = `${(await link!.ping()).toFixed(0)} ms`; } catch (e) { $("ping-out").textContent = (e as Error).message; } };
  $("forget").onclick = async () => {
    const id = String(info.id ?? "");
    const signedIn = !!loadAll()[id] && link?.t.kind !== "USB";
    let onDevice = false;
    if (signedIn && link) { try { await link.forget(); onDevice = true; } catch { /* an older device: revoke it there */ } }
    const all = loadAll(); delete all[id]; saveAll(all);
    $("paired-note").textContent = onDevice ? "Forgotten here and on the device: the next connection pairs again."
      : "Forgotten in this browser. Remove it on the device too: Settings > Remote > Authorized Devices.";
  };
  $<HTMLInputElement>("ota-file").onchange = (e) => { const f = (e.target as HTMLInputElement).files?.[0]; if (f) void flash(f); (e.target as HTMLInputElement).value = ""; };
  $("erase").onclick = async () => { if (await ask("ERASE", "Erase every file and setting the simulator saved in this browser? The next start is a first boot.")) await sim.eraseSim(); };

  /* the store */
  $<HTMLInputElement>("st-url").value = await defaultCatalog();
  $("st-load").onclick = () => void st.load();
  $<HTMLInputElement>("st-url").onkeydown = (e) => { if (e.key === "Enter") void st.load(); };
  $("st-q").oninput = () => st.render();

  /* the simulator's settings, and the hooks it calls back */
  sim.bindSettings(() => source === "sim", {
    line: addLine,
    stage: (s) => setState("busy", s),
    banner,
    shot: screenshot,
  });

  setSource("none");
  setState("idle", "Idle");
  showCtab("term");
  showSection(location.hash ? sectionFromHash() : (store.get(SECTION) ?? "display"), false);
  if (sim.wantsAutostart() && !blocker) void startSimulator();
}

void init();
