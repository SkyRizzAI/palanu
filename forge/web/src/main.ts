// Web Forge: remote a PalanuOS device from the browser. Connect over USB, Bluetooth or Wi-Fi/LAN, pair once with the
// device's code, then use its screen and keys, a shell, its files and its firmware. Everything runs in this page; the
// pairing token is kept in this browser's localStorage only.
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { Link, LinkError, LINK_ERR, type DeviceInfo, type Entry, type Transport } from "./link.ts";
import { connectBle, connectUsb, connectWs, support, WS_PORT } from "./transports.ts";

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const STORE = "palanu.webforge.links";
const ADDR = "palanu.webforge.addr";

type Pairing = { clientId: string; token: string; name: string; paired: string };
const loadAll = (): Record<string, Pairing> => { try { return JSON.parse(localStorage.getItem(STORE) || "{}"); } catch { return {}; } };
const saveAll = (m: Record<string, Pairing>) => { try { localStorage.setItem(STORE, JSON.stringify(m)); } catch { /* private window */ } };

let link: Link | null = null;
let info: DeviceInfo = {};
let shellOpen = false;
let term: Terminal | null = null;
let fit: FitAddon | null = null;
let cwd = "/home";

// ---------------------------------------------------------------- status

function status(text: string, kind: "idle" | "busy" | "ok" | "err" = "idle") {
  const s = $("status");
  s.textContent = text;
  s.dataset.kind = kind;
}

function setConnected(on: boolean) {
  document.body.dataset.connected = on ? "1" : "0";
  $("disconnect").hidden = !on;
  for (const id of ["c-usb", "c-ble", "c-ws"]) ($(id) as HTMLButtonElement).disabled = on;
}

// ---------------------------------------------------------------- connect, pair, auth

async function connect(make: () => Promise<Transport>) {
  try {
    status("Connecting…", "busy");
    const t = await make();
    link = new Link(t);
    link.onConsole = (d) => appendConsole(d);
    link.onScreen = paint; /* frames arrive only while the Screen tab has subscribed */
    t.onClose(() => { if (link?.t === t) dropped(); });
    status(`Connected over ${t.kind}: saying hello…`, "busy");
    info = await link.hello();
    if (!info.trusted) await trust();
    info = await link.hello();
    onReady();
  } catch (e) {
    const msg = (e as Error).message || String(e);
    if (/No port selected|User cancelled|cancelled the requestDevice/i.test(msg)) status("Not connected", "idle");
    else status(msg, "err");
    if (link && !document.body.dataset.ready) { await link.close().catch(() => {}); link = null; setConnected(false); }
  }
}

/** Makes the link trusted: the stored token for this device, or a pairing with the device's code. */
async function trust() {
  const id = String(info.id ?? "");
  const p = loadAll()[id];
  if (p) {
    try {
      status("Signing in…", "busy");
      await link!.auth(p.clientId, p.token);
      return;
    } catch (e) {
      if (e instanceof LinkError && e.code === LINK_ERR.busy) throw new Error("too many failed sign-ins: wait 30 seconds");
      status("The saved pairing was refused (revoked on the device?): pair again", "err");
    }
  }
  const pairing = await askPairing();
  const all = loadAll();
  all[id] = { ...pairing, name: String(info.name ?? id), paired: new Date().toISOString() };
  saveAll(all);
  await link!.auth(pairing.clientId, pairing.token);
}

function askPairing(): Promise<{ clientId: string; token: string }> {
  return new Promise((resolve, reject) => {
    const dlg = $("pair") as HTMLDialogElement;
    $("pair-device").textContent = String(info.name ?? "PalanuOS");
    ($("pair-code") as HTMLInputElement).value = "";
    $("pair-err").textContent = "";
    dlg.showModal();
    const form = $("pair-form") as HTMLFormElement;
    const cancel = () => { dlg.close(); cleanup(); reject(new Error("pairing cancelled")); };
    const submit = async (ev: SubmitEvent) => {
      ev.preventDefault();
      const code = ($("pair-code") as HTMLInputElement).value.trim();
      const name = (($("pair-name") as HTMLInputElement).value.trim() || "Web Forge").slice(0, 24);
      try {
        $("pair-err").textContent = "Pairing…";
        const r = await link!.pair(code, name);
        dlg.close(); cleanup(); resolve(r);
      } catch (e) {
        const le = e as LinkError;
        $("pair-err").textContent = le.code === LINK_ERR.pairClosed ? "No pairing window is open on the device: open it again" : le.message;
      }
    };
    const cleanup = () => { form.removeEventListener("submit", submit); $("pair-cancel").removeEventListener("click", cancel); };
    form.addEventListener("submit", submit);
    $("pair-cancel").addEventListener("click", cancel);
  });
}

function onReady() {
  document.body.dataset.ready = "1";
  setConnected(true);
  status(`${info.name ?? "PalanuOS"} · ${link!.t.kind}`, "ok");
  renderInfo();
  showTab(currentTab);
}

function dropped() {
  link = null;
  shellOpen = false;
  delete document.body.dataset.ready;
  setConnected(false);
  status("Disconnected", "err");
  term?.write("\r\n\x1b[2m[disconnected]\x1b[0m\r\n");
}

async function disconnect() {
  const l = link;
  link = null;
  if (l) { try { await l.subscribeScreen(false); await l.closeShell(); } catch { /* gone */ } await l.close().catch(() => {}); }
  shellOpen = false;
  delete document.body.dataset.ready;
  setConnected(false);
  status("Not connected", "idle");
}

// ---------------------------------------------------------------- tabs

let currentTab = "screen";

function showTab(name: string) {
  currentTab = name;
  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-tab]")) b.setAttribute("aria-selected", String(b.dataset.tab === name));
  for (const p of document.querySelectorAll<HTMLElement>("[data-panel]")) p.hidden = p.dataset.panel !== name;
  if (!link) return;
  void link.subscribeScreen(name === "screen");
  if (name === "terminal") openShell();
  if (name === "files") void listDir(cwd);
}

// ---------------------------------------------------------------- screen and keys

const canvas = () => $("screen") as HTMLCanvasElement;
let img: ImageData | null = null;

function paint(d: { x: number; y: number; w: number; h: number }) {
  if (!link) return;
  const s = link.screen, c = canvas();
  if (c.width !== s.w || c.height !== s.h) { c.width = s.w; c.height = s.h; img = null; }
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
  Home: 11, End: 12, PageUp: 13, PageDown: 14, Insert: 18,
};
export const SYM = { switch: 15, home: 16, keyboard: 17 } as const;

function onScreenKey(e: KeyboardEvent) {
  if (!link) return;
  const mods = (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0);
  if (e.metaKey) return; /* leave the browser's own shortcuts alone */
  if (KEY[e.key] !== undefined) { e.preventDefault(); void link.key(KEY[e.key], 0, mods); return; }
  if ([...e.key].length === 1) { e.preventDefault(); void link.key(1, e.key.codePointAt(0)!, mods & ~1); }
}

// ---------------------------------------------------------------- terminal

function ensureTerm() {
  if (term) return;
  term = new Terminal({ fontFamily: "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace", fontSize: 14, cursorBlink: true,
    theme: { background: "#0e1116", foreground: "#d7dde8", cursor: "#9ad1ff" } });
  fit = new FitAddon();
  term.loadAddon(fit);
  term.open($("term"));
  term.onData((d) => link?.ttyWrite(new TextEncoder().encode(d)));
  term.onResize(({ cols, rows }) => { if (shellOpen) void link?.resize(cols, rows); });
  new ResizeObserver(() => { if (currentTab === "terminal") fit?.fit(); }).observe($("term"));
}

function openShell() {
  ensureTerm();
  requestAnimationFrame(() => fit?.fit());
  if (!link || shellOpen) { term!.focus(); return; }
  link.onTty = (d) => term!.write(d);
  link.onTtyClose = () => { shellOpen = false; term!.write("\r\n\x1b[2m[shell ended — click Terminal to open another]\x1b[0m\r\n"); };
  void link.openShell(term!.cols, term!.rows);
  shellOpen = true;
  term!.focus();
}

// ---------------------------------------------------------------- files

const fmtSize = (n: number) => n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KB` : `${(n / 1048576).toFixed(1)} MB`;
const join = (a: string, b: string) => (a.endsWith("/") ? a : a + "/") + b;

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
      const tr = document.createElement("tr");
      const name = document.createElement("td");
      const btn = document.createElement("button");
      btn.className = "link";
      btn.textContent = (it.dir ? "▸ " : "") + it.name + (it.dir && it.name !== ".." ? "/" : "");
      btn.onclick = () => {
        if (it.name === "..") void listDir(path.replace(/\/[^/]+\/?$/, "") || "/");
        else if (it.dir) void listDir(join(path, it.name));
        else void download(join(path, it.name), it.name);
      };
      name.append(btn);
      const size = document.createElement("td");
      size.textContent = it.dir ? "" : fmtSize(it.size);
      size.className = "num";
      const act = document.createElement("td");
      act.className = "acts";
      const full = join(path, it.name);
      const button = (label: string, fn: () => void, cls = "") => {
        const b = document.createElement("button");
        b.textContent = label;
        if (cls) b.className = cls;
        b.onclick = fn;
        act.append(b);
      };
      if (!it.dir) button("Download", () => void download(full, it.name));
      if (!it.dir && TEXT_RE.test(it.name) && it.size <= 262144) button("Edit", () => void editFile(full));
      if (!it.dir && it.name.endsWith(".papp")) button("Install", () => void install(full));
      if (it.name !== ".." && path !== "/") {
        button("Rename", () => void renameEntry(path, it.name), "ghost");
        button("Delete", () => void removeEntry(full, it.dir), "ghost danger");
      }
      tr.append(name, size, act);
      tbody.append(tr);
    }
    if (!items.length) tbody.innerHTML = `<tr><td colspan="3" class="muted">Empty folder</td></tr>`;
  } catch (e) {
    tbody.innerHTML = `<tr><td colspan="3" class="err">${(e as Error).message}</td></tr>`;
  }
}

const TEXT_RE = /\.(txt|md|sh|conf|cfg|ini|json|toml|csv|log|c|h|js|ts|py|lua)$|^[^.]+$/i;

async function removeEntry(path: string, dir: boolean) {
  if (!link || !confirm(`Delete ${path}?${dir ? " (a folder must be empty)" : ""}`)) return;
  try {
    await link.remove(path);
    $("files-note").textContent = `Deleted ${path}`;
    void listDir(cwd);
  } catch (e) { $("files-note").textContent = (e as Error).message; }
}

async function renameEntry(dir: string, name: string) {
  const to = link ? prompt(`New name for ${name}`, name) : null;
  if (!link || !to || to === name || to.includes("/")) return;
  try {
    await link.rename(join(dir, name), join(dir, to));
    void listDir(cwd);
  } catch (e) { $("files-note").textContent = (e as Error).message; }
}

async function makeDir() {
  const name = link && cwd !== "/" ? prompt("New folder name") : null;
  if (!link || !name || name.includes("/")) return;
  try {
    await link.mkdir(join(cwd, name));
    void listDir(cwd);
  } catch (e) { $("files-note").textContent = (e as Error).message; }
}

async function install(path: string) {
  if (!link) return;
  $("files-note").textContent = `Installing ${path}…`;
  try {
    await link.install(path);
    $("files-note").textContent = `Installed ${path}: it is in the device's launcher and runs by its id`;
  } catch (e) { $("files-note").textContent = `Not installed: ${(e as Error).message}`; }
}

/* A text file in a dialog: read, edit, write back whole. */
async function editFile(path: string) {
  if (!link) return;
  const dlg = $("edit") as HTMLDialogElement, text = $("edit-text") as HTMLTextAreaElement;
  $("edit-path").textContent = path;
  $("edit-err").textContent = "";
  try {
    text.value = new TextDecoder().decode(await link.read(path));
  } catch (e) { $("files-note").textContent = (e as Error).message; return; }
  dlg.showModal();
  const form = $("edit-form") as HTMLFormElement;
  const done = () => { form.onsubmit = null; $("edit-cancel").onclick = null; };
  $("edit-cancel").onclick = () => { done(); dlg.close(); };
  form.onsubmit = async (ev) => {
    ev.preventDefault();
    try {
      await link!.write(path, new TextEncoder().encode(text.value));
      done();
      dlg.close();
      $("files-note").textContent = `Saved ${path}`;
      void listDir(cwd);
    } catch (e) { $("edit-err").textContent = (e as Error).message; }
  };
}

async function download(path: string, name: string) {
  if (!link) return;
  try {
    $("files-note").textContent = `Reading ${path}…`;
    const data = await link.read(path, (n) => ($("files-note").textContent = `Reading ${path}: ${fmtSize(n)}`));
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([data as BlobPart]));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    $("files-note").textContent = `Saved ${name} (${fmtSize(data.length)})`;
  } catch (e) { $("files-note").textContent = (e as Error).message; }
}

async function upload(files: FileList) {
  if (!link) return;
  for (const f of Array.from(files)) {
    const dest = join(cwd, f.name);
    try {
      const data = new Uint8Array(await f.arrayBuffer());
      await link.write(dest, data, (n) => ($("files-note").textContent = `Sending ${f.name}: ${Math.round((n / data.length) * 100)}%`));
      $("files-note").textContent = `Sent ${f.name} to ${dest}`;
    } catch (e) { $("files-note").textContent = `${f.name}: ${(e as Error).message}`; }
  }
  void listDir(cwd);
}

// ---------------------------------------------------------------- device

function renderInfo() {
  const dl = $("info");
  dl.innerHTML = "";
  const labels: Record<string, string> = { os: "System", ver: "Version", board: "Board", chip: "Chip", name: "Name", id: "Device id",
    uptime: "Uptime (s)", mem_free: "Internal RAM free", psram_free: "PSRAM free", slot: "Firmware slot", build: "Build", ota: "Updates" };
  for (const [k, v] of Object.entries(info)) {
    if (k === "trusted") continue;
    const dt = document.createElement("dt"), dd = document.createElement("dd");
    dt.textContent = labels[k] ?? k;
    dd.textContent = typeof v === "number" && /free/.test(k) ? fmtSize(v) : String(v);
    dl.append(dt, dd);
  }
  const p = loadAll()[String(info.id ?? "")];
  $("paired-note").textContent = p ? `Paired with “${p.name}” on ${new Date(p.paired).toLocaleString()}` :
    link?.t.kind === "USB" ? "Trusted over the cable (no pairing needed)." : "";
}

function appendConsole(d: Uint8Array) {
  const pre = $("console");
  pre.textContent = (pre.textContent + new TextDecoder().decode(d).replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "")).slice(-8000);
  pre.scrollTop = pre.scrollHeight;
}

async function flash(file: File) {
  if (!link) return;
  if (!confirmBox().checked) { $("ota-note").textContent = "Tick the box first: the device restarts into the new firmware."; return; }
  try {
    const img = new Uint8Array(await file.arrayBuffer());
    const bar = $("ota-bar") as HTMLProgressElement;
    bar.hidden = false;
    await link.ota(img, (d, t) => { bar.value = d / t; $("ota-note").textContent = `Writing ${fmtSize(d)} of ${fmtSize(t)}`; });
    $("ota-note").textContent = "Done: the device is restarting into the new firmware.";
  } catch (e) { $("ota-note").textContent = (e as Error).message; }
}
const confirmBox = () => $("ota-confirm") as HTMLInputElement;

// ---------------------------------------------------------------- wiring

function init() {
  const addr = $("ws-addr") as HTMLInputElement;
  try { addr.value = localStorage.getItem(ADDR) || ""; } catch { /* private window */ }
  addr.placeholder = `192.168.1.3 (port ${WS_PORT})`;

  ($("c-usb") as HTMLButtonElement).disabled = !support.usb;
  ($("c-ble") as HTMLButtonElement).disabled = !support.ble;
  if (!support.usb) $("c-usb").title = "Web Serial is in Chrome and Edge on a computer";
  if (!support.ble) $("c-ble").title = "Web Bluetooth is in Chrome and Edge";
  if (support.wsBlocked) $("ws-note").textContent = "This page is on https: browsers block ws:// to the device. Open Web Forge from the file or http://localhost.";

  $("c-usb").onclick = () => connect(connectUsb);
  $("c-ble").onclick = () => connect(connectBle);
  ($("ws-form") as HTMLFormElement).onsubmit = (e) => {
    e.preventDefault();
    if (!addr.value.trim()) { addr.focus(); return; }
    try { localStorage.setItem(ADDR, addr.value.trim()); } catch { /* private window */ }
    void connect(() => connectWs(addr.value));
  };
  $("disconnect").onclick = () => disconnect();
  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-tab]")) b.onclick = () => showTab(b.dataset.tab!);

  const c = canvas();
  c.addEventListener("keydown", onScreenKey);
  c.addEventListener("click", () => c.focus());
  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-sym]")) b.onclick = () => { void link?.key(Number(b.dataset.sym)); c.focus(); };
  $("refresh").onclick = () => void link?.refreshScreen();

  $("files-up").onclick = () => ($("files-input") as HTMLInputElement).click();
  ($("files-input") as HTMLInputElement).onchange = (e) => { const f = (e.target as HTMLInputElement).files; if (f?.length) void upload(f); (e.target as HTMLInputElement).value = ""; };
  $("files-refresh").onclick = () => void listDir(cwd);
  $("files-mkdir").onclick = () => void makeDir();
  $("files-root").onclick = () => void listDir("/");

  $("ping").onclick = async () => { try { $("ping-out").textContent = `${(await link!.ping()).toFixed(0)} ms`; } catch (e) { $("ping-out").textContent = (e as Error).message; } };
  $("forget").onclick = () => {
    const all = loadAll(); delete all[String(info.id ?? "")]; saveAll(all);
    $("paired-note").textContent = "Pairing forgotten in this browser. Revoke it on the device too: link revoke";
  };
  ($("ota-file") as HTMLInputElement).onchange = (e) => { const f = (e.target as HTMLInputElement).files?.[0]; if (f) void flash(f); (e.target as HTMLInputElement).value = ""; };

  setConnected(false);
  status("Not connected", "idle");
  showTab("screen");
}

init();
