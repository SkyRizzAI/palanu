// Web Forge Simulator: PalanuOS running in this page. The system is the same C code as on the board (WebAssembly build of
// platform/web); everything the board would have around it (screen, keys, battery, sensors, pins, Wi-Fi, network) is
// simulated here and can be changed while it runs. Nothing leaves the browser except the bridge you turn on.
import { defaults, DEFAULT_APS, DEFAULT_HOTKEYS, load, save, type Config, type Hotkey } from "./config.ts";
import { Engine, installBridge } from "./engine.ts";
import { buildKeyboard } from "./keymap.ts";
import { run as runScript, runStep, parse, type Host } from "./script.ts";

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const cfg: Config = load();
const commit = () => save(cfg);
let engine: Engine | null = null;
let booted = performance.now();

// ---------------------------------------------------------------- status and log

function status(text: string, kind: "busy" | "ok" | "err" = "busy") {
  $("status").textContent = text;
  $("status").dataset.kind = kind;
}
function banner(text: string) { $("banner").textContent = text; }

interface Line { n: number; t: string; err: boolean; ts: number }
const lines: Line[] = [];
let seq = 0;
let paused = false;
const logEl = $("log");
const PROBLEM = /\b(error|fail(ed|ure)?|panic|assert|abort|refused|denied|cannot|overflow)\b/i;

function addLine(t: string, err: boolean) {
  const l: Line = { n: ++seq, t, err: err || PROBLEM.test(t), ts: performance.now() - booted };
  lines.push(l);
  if (lines.length > cfg.log.max) lines.splice(0, lines.length - cfg.log.max);
  if (!paused) dirtyLog = true;
}
let dirtyLog = true;
function renderLog() {
  if (!dirtyLog) return;
  dirtyLog = false;
  const f = ($("l-filter") as HTMLInputElement).value.toLowerCase();
  const only = ($("l-err") as HTMLInputElement).checked;
  const stick = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 8;
  const shown = lines.filter((l) => (!only || l.err) && (!f || l.t.toLowerCase().includes(f)));
  const frag = document.createDocumentFragment();
  for (const l of shown.slice(-1500)) {
    const d = document.createElement("div");
    d.className = "ln" + (l.err ? " bad" : "");
    d.textContent = `${(l.ts / 1000).toFixed(2).padStart(7)}  ${l.t}`;
    frag.append(d);
  }
  logEl.replaceChildren(frag);
  $("l-count").textContent = `${shown.length} of ${lines.length} lines`;
  if (stick) logEl.scrollTop = logEl.scrollHeight;
}

// ---------------------------------------------------------------- screen

const canvas = $<HTMLCanvasElement>("screen");
const ctx = canvas.getContext("2d")!;
let fpsCount = 0, fpsAt = performance.now(), fps = 0;

function applyScreen() {
  const s = cfg.screen;
  canvas.style.imageRendering = s.smooth ? "auto" : "pixelated";
  canvas.style.filter = [`brightness(${s.brightness / 100})`, s.grayscale ? "grayscale(1)" : "", s.invert ? "invert(1)" : ""].filter(Boolean).join(" ");
  if (s.scale === "fit") { canvas.style.width = "100%"; canvas.style.maxWidth = ""; }
  else { canvas.style.width = `${(engine?.w ?? 320) * Number(s.scale)}px`; canvas.style.maxWidth = "100%"; }
  $("pixgrid").dataset.on = s.grid ? "1" : "0";
  $("shell").dataset.bezel = s.bezel ? "1" : "0";
}

function loop() {
  if (engine) {
    const f = engine.frame();
    if (f) { ctx.putImageData(f, 0, 0); fpsCount++; }
    const now = performance.now();
    if (now - fpsAt >= 1000) { fps = Math.round((fpsCount * 1000) / (now - fpsAt)); fpsCount = 0; fpsAt = now; }
  }
  renderLog();
  requestAnimationFrame(loop);
}

function screenshot() {
  canvas.toBlob((b) => {
    if (!b) return;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(b);
    a.download = `palanu-${new Date().toISOString().replace(/[:.]/g, "-")}.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  });
}

// ---------------------------------------------------------------- input

const NAMED: Record<string, string> = {
  Enter: "enter", Escape: "esc", Backspace: "backspace", Tab: "tab", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
  Home: "home", End: "end", PageUp: "pgup", PageDown: "pgdn", Delete: "delete", F1: "switch", F2: "home-app",
};
const PAL_KEY_CHAR = 1;
const MOD = { shift: 1, ctrl: 2, alt: 4 };

function forwardKey(ev: KeyboardEvent): boolean {
  if (!engine || ev.isComposing) return false;
  const named = NAMED[ev.key];
  if (named) { engine.key(named); return true; }
  if (ev.key.length >= 1 && [...ev.key].length === 1) {
    const mods = (ev.ctrlKey ? MOD.ctrl : 0) | (ev.altKey ? MOD.alt : 0);
    const cp = ev.key.codePointAt(0)!;
    if (mods) engine.keySym(PAL_KEY_CHAR, cp, mods | (ev.shiftKey ? MOD.shift : 0));
    else if (!ev.metaKey) engine.keySym(PAL_KEY_CHAR, cp, 0);
    else return false;
    recordKey(ev, mods !== 0);
    return true;
  }
  return false;
}

// Hotkeys: "Ctrl+Shift+S". The key name is event.key, letters upper-cased.
function combo(ev: KeyboardEvent): string {
  const k = ev.key.length === 1 ? ev.key.toUpperCase() : ev.key;
  if (["Control", "Shift", "Alt", "Meta"].includes(k)) return "";
  return [ev.ctrlKey ? "Ctrl" : "", ev.altKey ? "Alt" : "", ev.shiftKey ? "Shift" : "", ev.metaKey ? "Meta" : "", k === " " ? "Space" : k].filter(Boolean).join("+");
}

let capturing: ((c: string | null) => void) | null = null;

addEventListener("keydown", (ev) => {
  if (capturing) {
    ev.preventDefault();
    if (ev.key === "Escape") { const f = capturing; capturing = null; f(null); return; }
    const c = combo(ev);
    if (c) { const f = capturing; capturing = null; f(c); }
    return;
  }
  const t = ev.target as HTMLElement;
  const typing = t instanceof HTMLInputElement || t instanceof HTMLTextAreaElement || t instanceof HTMLSelectElement;
  const c = combo(ev);
  const hk = c && cfg.keys.hotkeys.find((h) => h.combo === c);
  if (hk && !(typing && !ev.ctrlKey && !ev.altKey)) { ev.preventDefault(); void runOne(hk.action); return; }
  if (typing) return;
  if (t === canvas || (cfg.keys.captureAll && !(ev.ctrlKey && /^[a-z]$/i.test(ev.key) && !ev.altKey) && !ev.metaKey)) {
    if (forwardKey(ev)) ev.preventDefault();
  }
}, true);

// ---------------------------------------------------------------- key sequences

const host: Host = {
  cmd: async (l) => { await engine?.type(l); engine?.key("enter"); recordText(l, true); },
  type: async (t) => { await engine?.type(t); },
  key: (n) => engine?.key(n) ?? false,
  tap: async (r, c) => { engine?.hw(r, c, true); await new Promise((ok) => setTimeout(ok, 80)); engine?.hw(r, c, false); },
  battery: (p) => { cfg.hw.noBattery = p < 0; if (p >= 0) cfg.hw.battery = p; engine?.battery(p); syncHardware(); },
  sensor: (i, c) => { cfg.hw.sensorC[i] = c; engine?.sensor(i, c); syncHardware(); },
  gpio: (i, l) => { cfg.hw.gpioIn[i] = l; engine?.setGpio(i, l); syncHardware(); },
  shot: screenshot,
  restart: () => restart(),
  note: (t) => addLine(`-- ${t}`, false),
};

async function runOne(line: string) {
  try { for (const s of parse(line)) await runStep(host, s, () => false); } catch (e) { banner((e as Error).message); }
}

let stopFlag = false, running = false;
const sqText = $<HTMLTextAreaElement>("sq-text"), sqPick = $<HTMLSelectElement>("sq-pick");

function fillSequences(select?: string) {
  const names = Object.keys(cfg.sequences);
  sqPick.replaceChildren(...names.map((n) => Object.assign(document.createElement("option"), { value: n, textContent: n })));
  const pick = select && names.includes(select) ? select : names[0];
  if (pick) { sqPick.value = pick; sqText.value = cfg.sequences[pick]; } else sqText.value = "";
}

let recording = false, recLast = 0, recBuf: string[] = [];
function recGap() {
  const now = performance.now();
  if (recLast && now - recLast > 400) recBuf.push(`wait ${Math.round((now - recLast) / 50) * 50}`);
  recLast = now;
}
function recordKey(ev: KeyboardEvent, chord: boolean) {
  if (!recording) return;
  recGap();
  recBuf.push(chord ? `key ${ev.ctrlKey ? "ctrl-" : "alt-"}${ev.key.toLowerCase()}` : `type ${ev.key}`);
}
function recordText(t: string, enter: boolean) { if (recording) { recGap(); recBuf.push(`${enter ? "cmd" : "type"} ${t}`); } }

// ---------------------------------------------------------------- hardware panels

const el = <T extends HTMLElement>(tag: string, cls?: string, text?: string): T => Object.assign(document.createElement(tag), { className: cls ?? "", textContent: text ?? "" }) as T;

let leds: HTMLElement[] = [];
function buildHardware() {
  const h = cfg.hw;
  ($("h-bat") as HTMLInputElement).value = String(h.battery);
  $("h-bat-o").textContent = h.noBattery ? "none" : `${h.battery}%`;
  ($("h-nobat") as HTMLInputElement).checked = h.noBattery;
  ($("h-leds") as HTMLInputElement).value = String(h.leds);
  ($("h-sens") as HTMLInputElement).value = String(h.sensors);
  ($("h-gpios") as HTMLInputElement).value = String(h.gpios);
  ($("h-i2c") as HTMLInputElement).checked = h.i2c;
  $("h-led-card").hidden = h.leds === 0;
  $("h-sen-card").hidden = h.sensors === 0;
  $("h-gpio-card").hidden = h.gpios === 0;
  $("h-i2c-card").hidden = !h.i2c;
  leds = [];
  $("h-led").replaceChildren(...Array.from({ length: h.leds }, (_, i) => {
    const chip = el("span", "chip led"); chip.append(el("i", "dot"), el("span", "", `LED ${i + 1}`), el("code", "", "off")); leds.push(chip); return chip;
  }));
  $("leds").replaceChildren(...Array.from({ length: h.leds }, () => el("i", "dot")));
  $("h-sen").replaceChildren(...Array.from({ length: h.sensors }, (_, i) => {
    const row = el("label", "field"); row.append(`Temperature ${i + 1}`);
    const r = Object.assign(document.createElement("input"), { type: "range", min: "-20", max: "90", step: "0.5", value: String(h.sensorC[i]) });
    const o = el("output", "", `${h.sensorC[i]} °C`);
    r.oninput = () => { h.sensorC[i] = Number(r.value); o.textContent = `${r.value} °C`; engine?.sensor(i, h.sensorC[i]); commit(); };
    row.append(r, o); return row;
  }));
  $("h-gpio").replaceChildren(...Array.from({ length: h.gpios }, (_, i) => {
    const b = el<HTMLButtonElement>("button", "chip pin"); b.type = "button"; b.dataset.i = String(i); b.textContent = `Pin ${10 + i}: 0`;
    b.onclick = () => { h.gpioIn[i] = engine ? 1 - engine.gpio(i) : 1 - h.gpioIn[i]; engine?.setGpio(i, h.gpioIn[i]); commit(); };
    return b;
  }));
  buildI2c();
}

function buildI2c() {
  $("h-i2c-list").replaceChildren(...cfg.hw.i2cAddrs.map((a) => {
    const c = el<HTMLButtonElement>("button", "chip", `0x${a.toString(16).padStart(2, "0")}  ✕`); c.type = "button"; c.title = "Remove";
    c.onclick = () => { cfg.hw.i2cAddrs = cfg.hw.i2cAddrs.filter((x) => x !== a); engine?.i2c(a, false); commit(); buildI2c(); };
    return c;
  }));
}

function syncHardware() { buildHardware(); commit(); }

function pollHardware() {
  if (!engine) return;
  for (let i = 0; i < leds.length; i++) {
    const v = engine.led(i), hex = "#" + (v & 0xffffff).toString(16).padStart(6, "0");
    (leds[i].firstElementChild as HTMLElement).style.background = v ? hex : "";
    leds[i].lastElementChild!.textContent = v ? hex : "off";
    const d = $("leds").children[i] as HTMLElement | undefined;
    if (d) { d.style.background = v ? hex : ""; d.style.boxShadow = v ? `0 0 8px 2px ${hex}` : ""; }
  }
  document.querySelectorAll<HTMLButtonElement>("#h-gpio button").forEach((b) => { b.textContent = `Pin ${10 + Number(b.dataset.i)}: ${engine!.gpio(Number(b.dataset.i))}`; });
  const rs = ["off", "starting", "on", "stopping", "failed"];
  $("n-wifi-radio").textContent = rs[engine.radio(0)] ?? "?";
  $("n-ble-radio").textContent = rs[engine.radio(1)] ?? "?";
  $("n-wifi-link").textContent = engine.wifiConnected() ? `connected (${cfg.wifi.ip})` : "not connected";
  $("p-frames").textContent = String(engine.frames);
  $("p-fps").textContent = `${fps} per second`;
  $("p-up").textContent = `${Math.round((performance.now() - booted) / 1000)} s`;
}

// ---------------------------------------------------------------- network panels

function buildAps() {
  $("n-aps").replaceChildren(...cfg.wifi.aps.map((a, i) => {
    const tr = el("tr");
    const cell = (child: HTMLElement) => { const td = el("td"); td.append(child); tr.append(td); };
    const inp = (v: string | number, type: string, w: number, set: (v: string) => void) => {
      const x = Object.assign(document.createElement("input"), { type, value: String(v), size: w }); x.autocomplete = "off"; x.spellcheck = false;
      x.oninput = () => { set(x.value); wifiChanged(); }; return x;
    };
    cell(inp(a.ssid, "text", 14, (v) => (a.ssid = v.slice(0, 32))));
    cell(inp(a.rssi, "number", 4, (v) => (a.rssi = Math.max(-100, Math.min(0, Number(v) || -60)))));
    cell(inp(a.channel, "number", 3, (v) => (a.channel = Math.max(1, Math.min(165, Number(v) || 1)))));
    const sec = Object.assign(document.createElement("input"), { type: "checkbox", checked: a.secure }); sec.onchange = () => { a.secure = sec.checked; wifiChanged(); }; cell(sec);
    cell(inp(a.password, "text", 12, (v) => (a.password = v)));
    const del = el<HTMLButtonElement>("button", "ghost", "✕"); del.type = "button"; del.onclick = () => { cfg.wifi.aps.splice(i, 1); buildAps(); wifiChanged(); }; cell(del);
    return tr;
  }));
}
function wifiChanged() { commit(); engine?.applyWifi(cfg); }

function buildNetwork() {
  buildAps();
  ($("n-ip") as HTMLInputElement).value = cfg.wifi.ip;
  ($("n-refuse") as HTMLInputElement).checked = cfg.wifi.refuse;
  ($("b-mode") as HTMLSelectElement).value = cfg.bridge.mode;
  ($("b-proxy") as HTMLInputElement).value = cfg.bridge.proxy;
  $("b-proxy-row").hidden = cfg.bridge.mode !== "proxy";
}

// ---------------------------------------------------------------- hotkey table

function buildHotkeys() {
  $("hk-body").replaceChildren(...cfg.keys.hotkeys.map((h, i) => {
    const tr = el("tr");
    const keyBtn = el<HTMLButtonElement>("button", "ghost", h.combo || "Press…"); keyBtn.type = "button";
    keyBtn.onclick = () => { $("cap").hidden = false; capturing = (c) => { $("cap").hidden = true; if (c) { h.combo = c; commit(); buildHotkeys(); } }; };
    const act = Object.assign(document.createElement("input"), { type: "text", value: h.action, size: 22 }); act.autocomplete = "off"; act.spellcheck = false;
    act.oninput = () => { h.action = act.value; commit(); };
    const del = el<HTMLButtonElement>("button", "ghost", "✕"); del.type = "button"; del.onclick = () => { cfg.keys.hotkeys.splice(i, 1); commit(); buildHotkeys(); };
    for (const c of [keyBtn, act, del]) { const td = el("td"); td.append(c); tr.append(td); }
    return tr;
  }));
}

// ---------------------------------------------------------------- restart, erase

function restart() { engine?.persist(); setTimeout(() => location.reload(), 150); }

// ---------------------------------------------------------------- wiring

function bind() {
  const $i = (id: string) => $(id) as HTMLInputElement;
  const s = cfg.screen;
  $i("d-bright").value = String(s.brightness); $("d-bright-o").textContent = `${s.brightness}%`;
  ($("d-scale") as HTMLSelectElement).value = s.scale;
  for (const [id, key] of [["d-smooth", "smooth"], ["d-grid", "grid"], ["d-gray", "grayscale"], ["d-invert", "invert"], ["d-bezel", "bezel"]] as const) {
    $i(id).checked = s[key];
    $i(id).onchange = () => { s[key] = $i(id).checked; commit(); applyScreen(); };
  }
  ($("d-scale") as HTMLSelectElement).onchange = () => { s.scale = ($("d-scale") as HTMLSelectElement).value; commit(); applyScreen(); };
  $i("d-res").value = s.res;
  $("d-res-apply").onclick = () => {
    const m = /^(\d{2,3})x(\d{2,3})$/.exec($i("d-res").value.trim());
    if (!m || +m[1] < 64 || +m[1] > 320 || +m[2] < 48 || +m[2] > 240) return banner("The screen is WIDTHxHEIGHT, from 64x48 up to 320x240");
    banner(""); s.res = `${m[1]}x${m[2]}`; commit(); restart();
  };
  $i("d-bright").oninput = () => { s.brightness = Number($i("d-bright").value); $("d-bright-o").textContent = `${s.brightness}%`; commit(); applyScreen(); };

  $i("k-all").checked = cfg.keys.captureAll; $i("k-all").onchange = () => { cfg.keys.captureAll = $i("k-all").checked; commit(); };
  $i("k-sticky").checked = cfg.keys.sticky; $i("k-sticky").onchange = () => { cfg.keys.sticky = $i("k-sticky").checked; commit(); };
  $("k-form").onsubmit = (e) => { e.preventDefault(); const f = $i("k-line"); host.cmd(f.value); f.value = ""; };

  for (const b of document.querySelectorAll<HTMLButtonElement>("[data-key]")) b.onclick = () => { engine?.key(b.dataset.key!); canvas.focus(); };
  const kb = $("kb"), kbBtn = $("kb-toggle");
  kbBtn.onclick = () => { const on = kb.hidden; kb.hidden = !on; kbBtn.setAttribute("aria-pressed", String(on)); };
  buildKeyboard(kb, { down: (r, c) => engine?.hw(r, c, true), up: (r, c) => engine?.hw(r, c, false), sticky: () => cfg.keys.sticky });

  $("hk-add").onclick = () => { cfg.keys.hotkeys.push({ combo: "", action: "key enter" }); commit(); buildHotkeys(); ($("hk-body").lastElementChild!.querySelector("button") as HTMLButtonElement).click(); };
  $("hk-reset").onclick = () => { cfg.keys.hotkeys = DEFAULT_HOTKEYS.map((h: Hotkey) => ({ ...h })); commit(); buildHotkeys(); };

  fillSequences();
  sqPick.onchange = () => { sqText.value = cfg.sequences[sqPick.value] ?? ""; };
  sqText.oninput = () => { if (sqPick.value) { cfg.sequences[sqPick.value] = sqText.value; commit(); } };
  $("sq-new").onclick = () => {
    let n = 1; while (cfg.sequences[`Sequence ${n}`]) n++;
    cfg.sequences[`Sequence ${n}`] = "# steps, one per line\ncmd help"; commit(); fillSequences(`Sequence ${n}`);
  };
  $("sq-del").onclick = () => { if (sqPick.value) { delete cfg.sequences[sqPick.value]; commit(); fillSequences(); } };
  const msg = $("sq-msg");
  $("sq-run").onclick = async () => {
    if (running) return;
    running = true; stopFlag = false; msg.textContent = ""; banner("");
    ($("sq-run") as HTMLButtonElement).disabled = true; ($("sq-stop") as HTMLButtonElement).disabled = false;
    try { await runScript(host, sqText.value, Math.max(1, Number($i("sq-rep").value) || 1), () => stopFlag, (st) => (msg.textContent = `line ${st.line}: ${st.verb}`)); msg.textContent = stopFlag ? "stopped" : "done"; }
    catch (e) { msg.textContent = (e as Error).message; }
    running = false; ($("sq-run") as HTMLButtonElement).disabled = false; ($("sq-stop") as HTMLButtonElement).disabled = true;
  };
  $("sq-stop").onclick = () => { stopFlag = true; };
  $("sq-rec").onclick = () => {
    recording = !recording;
    $("sq-rec").setAttribute("aria-pressed", String(recording)); $("sq-rec").textContent = recording ? "Stop recording" : "Record";
    if (recording) { recBuf = []; recLast = 0; canvas.focus(); }
    else if (recBuf.length) { sqText.value = (sqText.value.trimEnd() ? sqText.value.trimEnd() + "\n" : "") + mergeTypes(recBuf).join("\n"); sqText.dispatchEvent(new Event("input")); }
  };

  buildHardware();
  $i("h-bat").oninput = () => { cfg.hw.battery = Number($i("h-bat").value); cfg.hw.noBattery = false; $i("h-nobat").checked = false; $("h-bat-o").textContent = `${cfg.hw.battery}%`; engine?.battery(cfg.hw.battery); commit(); };
  $i("h-nobat").onchange = () => { cfg.hw.noBattery = $i("h-nobat").checked; $("h-bat-o").textContent = cfg.hw.noBattery ? "none" : `${cfg.hw.battery}%`; engine?.battery(cfg.hw.noBattery ? -1 : cfg.hw.battery); commit(); };
  $("h-apply").onclick = () => {
    const n = (id: string) => Math.max(0, Math.min(8, Math.floor(Number($i(id).value) || 0)));
    cfg.hw.leds = n("h-leds"); cfg.hw.sensors = n("h-sens"); cfg.hw.gpios = n("h-gpios"); cfg.hw.i2c = $i("h-i2c").checked;
    commit(); restart();
  };
  $("h-i2c-form").onsubmit = (e) => {
    e.preventDefault();
    const v = parseInt($i("h-i2c-addr").value.trim().replace(/^0x/i, ""), 16);
    if (!(v >= 0x08 && v <= 0x77)) return banner("I2C addresses run from 0x08 to 0x77");
    banner(""); if (!cfg.hw.i2cAddrs.includes(v)) cfg.hw.i2cAddrs.push(v);
    engine?.i2c(v, true); commit(); buildI2c(); $i("h-i2c-addr").value = "";
  };

  buildNetwork();
  $("n-add").onclick = () => { cfg.wifi.aps.push({ ssid: "NewNetwork", rssi: -55, channel: 6, secure: true, password: "" }); buildAps(); wifiChanged(); };
  $("n-reset").onclick = () => { cfg.wifi.aps = DEFAULT_APS.map((a) => ({ ...a })); buildAps(); wifiChanged(); };
  $i("n-ip").onchange = () => { if (/^\d{1,3}(\.\d{1,3}){3}$/.test($i("n-ip").value)) { cfg.wifi.ip = $i("n-ip").value; wifiChanged(); } else $i("n-ip").value = cfg.wifi.ip; };
  $i("n-refuse").onchange = () => { cfg.wifi.refuse = $i("n-refuse").checked; wifiChanged(); };
  $("n-drop").onclick = () => engine?.wifiDrop();
  ($("b-mode") as HTMLSelectElement).onchange = () => { cfg.bridge.mode = ($("b-mode") as HTMLSelectElement).value as Config["bridge"]["mode"]; $("b-proxy-row").hidden = cfg.bridge.mode !== "proxy"; commit(); };
  $i("b-proxy").onchange = () => { cfg.bridge.proxy = $i("b-proxy").value; commit(); };

  buildHotkeys();

  $i("l-filter").oninput = () => { dirtyLog = true; };
  $i("l-err").onchange = () => { dirtyLog = true; };
  $i("l-wrap").checked = cfg.log.wrap; logEl.dataset.wrap = cfg.log.wrap ? "1" : "0";
  $i("l-wrap").onchange = () => { cfg.log.wrap = $i("l-wrap").checked; logEl.dataset.wrap = cfg.log.wrap ? "1" : "0"; commit(); };
  $("l-pause").onclick = () => { paused = !paused; $("l-pause").setAttribute("aria-pressed", String(paused)); $("l-pause").textContent = paused ? "Resume" : "Pause"; dirtyLog = !paused; };
  $("l-clear").onclick = () => { lines.length = 0; dirtyLog = true; };
  $("l-save").onclick = () => {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([lines.map((l) => `${(l.ts / 1000).toFixed(2)} ${l.t}`).join("\n") + "\n"], { type: "text/plain" }));
    a.download = "palanu-sim.log"; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  };

  $("restart").onclick = () => restart();
  $("shot").onclick = screenshot;
  $("erase").onclick = async () => { await Engine.eraseData(); location.reload(); };

  const tabs = [...document.querySelectorAll<HTMLButtonElement>("#tabs button")];
  const show = (name: string) => {
    for (const t of tabs) t.setAttribute("aria-selected", String(t.dataset.tab === name));
    for (const p of document.querySelectorAll<HTMLElement>(".panel")) p.hidden = p.dataset.panel !== name;
    try { localStorage.setItem("palanu.sim.tab", name); } catch { /* private window */ }
    if (name === "log") dirtyLog = true;
  };
  for (const t of tabs) t.onclick = () => show(t.dataset.tab!);
  let first = "display";
  try { first = localStorage.getItem("palanu.sim.tab") || first; } catch { /* private window */ }
  show(tabs.some((t) => t.dataset.tab === first) ? first : "display");
  applyScreen();
}

function mergeTypes(buf: string[]): string[] {
  const out: string[] = [];
  for (const l of buf) {
    const last = out[out.length - 1];
    if (l.startsWith("type ") && last?.startsWith("type ")) out[out.length - 1] = last + l.slice(5);
    else out.push(l);
  }
  return out;
}

// ---------------------------------------------------------------- start

async function start() {
  bind();
  requestAnimationFrame(loop);
  $("p-iso").textContent = crossOriginIsolated ? "yes" : "no";
  if (!crossOriginIsolated && "serviceWorker" in navigator && isSecureContext && !sessionStorage.getItem("palanu.sim.coi")) {
    /* a host that cannot send the isolation headers: a service worker adds them, then the page reloads once */
    try {
      sessionStorage.setItem("palanu.sim.coi", "1");
      await navigator.serviceWorker.register("coi-sw.js");
      status("Preparing", "busy");
      await navigator.serviceWorker.ready;
      location.reload();
      return;
    } catch { /* fall through to the message below */ }
  }
  if (!crossOriginIsolated) {
    status("Cannot start", "err");
    banner("This page must be served with cross-origin isolation (COOP and COEP headers) for the system to run its tasks in workers. Start it with ./pal web, or serve it with those headers (a secure https page can use the bundled coi-sw.js).");
    return;
  }
  installBridge(() => cfg, (s) => addLine(`[net] ${s}`, false));
  try {
    booted = performance.now();
    engine = await Engine.boot(cfg, addLine, (s) => status(s, "busy"));
    engine.applyWifi(cfg);
    for (const a of [0x48, 0x68]) if (!cfg.hw.i2cAddrs.includes(a)) engine.i2c(a, false);
    for (const a of cfg.hw.i2cAddrs) engine.i2c(a, true);
    cfg.hw.sensorC.forEach((c, i) => i < cfg.hw.sensors && engine!.sensor(i, c));
    cfg.hw.gpioIn.forEach((l, i) => i < cfg.hw.gpios && engine!.setGpio(i, l));
    canvas.width = engine.w; canvas.height = engine.h;
    canvas.style.aspectRatio = `${engine.w} / ${engine.h}`;
    $("pixgrid").style.backgroundSize = `calc(100% / ${engine.w}) calc(100% / ${engine.h})`;
    applyScreen();
    document.body.dataset.state = "running";
    status("Running", "ok");
    canvas.focus();
    setInterval(pollHardware, 250);
  } catch (e) {
    status("Failed to start", "err");
    banner((e as Error).message || String(e));
  }
}

void start();
void defaults;
