// The simulator in Palanu Forge: PalanuOS built to WebAssembly (platform/web), running in this page. The board around it
// (screen, keys, battery, sensors, pins, Wi-Fi, the socket bridge) is simulated and changes while it runs. The page reaches
// its Palanu Link through a virtual cable (pal_web_link_*), so Files, Terminal and System work as they do for a board on USB.
import { DEFAULT_APS, DEFAULT_HOTKEYS, load, save, type Ap, type Config, type Hotkey } from "./config.ts";
import { Engine, installBridge } from "./engine.ts";
import { buildKeyboard } from "./keymap.ts";
import { run as runScript, runStep, parse, type Host } from "./script.ts";
import type { Transport } from "../link.ts";

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const $i = (id: string) => $<HTMLInputElement>(id);
const el = <T extends HTMLElement>(tag: string, cls?: string, text?: string): T =>
  Object.assign(document.createElement(tag), { className: cls ?? "", textContent: text ?? "" }) as T;

export const cfg: Config = load();
const commit = () => save(cfg);
export let engine: Engine | null = null;
let booted = 0;
let active = () => false; /* the page's source is the simulator */

export interface SimHooks {
  line(text: string, err: boolean): void;
  stage(text: string): void;
  banner(text: string): void;
  shot(): void;
}
let hooks: SimHooks = { line: () => {}, stage: () => {}, banner: () => {}, shot: () => {} };

// ---------------------------------------------------------------- can it run here

const AUTOSTART = "palanu.forge.autostart";

/** Why the simulator cannot start in this page, or "" when it can. */
export function simBlocker(): string {
  if (location.protocol === "file:") return "The simulator runs when the page is served: start it with ./pal web, then open http://localhost:5174.";
  if (!crossOriginIsolated && !("serviceWorker" in navigator && isSecureContext))
    return "The simulator needs cross-origin isolation (COOP and COEP headers). ./pal web sets them.";
  return "";
}

/** Asks for a start after the next load (Restart, or the one reload that turns isolation on). */
export function startAfterReload() {
  try { sessionStorage.setItem(AUTOSTART, "1"); } catch { /* private window */ }
}
export function wantsAutostart(): boolean {
  try { const v = sessionStorage.getItem(AUTOSTART) === "1"; sessionStorage.removeItem(AUTOSTART); return v; } catch { return false; }
}

// ---------------------------------------------------------------- screen

const canvas = () => $<HTMLCanvasElement>("screen");
let fpsCount = 0, fpsAt = 0, fps = 0;

export function applyScreen() {
  const s = cfg.screen, c = canvas();
  c.style.imageRendering = s.smooth ? "auto" : "pixelated";
  c.style.filter = active() ? [`brightness(${s.brightness / 100})`, s.grayscale ? "grayscale(1)" : "", s.invert ? "invert(1)" : ""].filter(Boolean).join(" ") : "";
  if (s.scale === "fit" || !active()) { c.style.width = "100%"; c.style.maxWidth = ""; }
  else { c.style.width = `${(engine?.w ?? 320) * Number(s.scale)}px`; c.style.maxWidth = "100%"; }
  $("pixgrid").dataset.on = s.grid && active() ? "1" : "0";
  $("bezel").dataset.on = s.bezel || !active() ? "1" : "0";
}

function loop() {
  if (engine && active()) {
    const f = engine.frame();
    if (f) { canvas().getContext("2d")!.putImageData(f, 0, 0); fpsCount++; }
    const now = performance.now();
    if (now - fpsAt >= 1000) { fps = Math.round((fpsCount * 1000) / (now - fpsAt)); fpsCount = 0; fpsAt = now; }
  }
  requestAnimationFrame(loop);
}

// ---------------------------------------------------------------- keys

const NAMED: Record<string, string> = {
  Enter: "enter", Escape: "esc", Backspace: "backspace", Tab: "tab", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
  Home: "home", End: "end", PageUp: "pgup", PageDown: "pgdn", Delete: "delete", F1: "switch", F2: "home-app",
};
const PAL_KEY_CHAR = 1;
const MOD = { shift: 1, ctrl: 2, alt: 4 };

export function simKey(name: string) { engine?.key(name); }

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

/* Hotkeys: "Ctrl+Shift+S". The key name is event.key, letters upper-cased. */
function combo(ev: KeyboardEvent): string {
  const k = ev.key.length === 1 ? ev.key.toUpperCase() : ev.key;
  if (["Control", "Shift", "Alt", "Meta"].includes(k)) return "";
  return [ev.ctrlKey ? "Ctrl" : "", ev.altKey ? "Alt" : "", ev.shiftKey ? "Shift" : "", ev.metaKey ? "Meta" : "", k === " " ? "Space" : k].filter(Boolean).join("+");
}

let capturing: ((c: string | null) => void) | null = null;

function onKey(ev: KeyboardEvent) {
  if (capturing) {
    ev.preventDefault();
    if (ev.key === "Escape") { const f = capturing; capturing = null; f(null); return; }
    const c = combo(ev);
    if (c) { const f = capturing; capturing = null; f(c); }
    return;
  }
  if (!active()) return;
  const t = ev.target as HTMLElement;
  const typing = t instanceof HTMLInputElement || t instanceof HTMLTextAreaElement || t instanceof HTMLSelectElement || !!t.closest?.(".term, dialog");
  const c = combo(ev);
  const hk = c && cfg.keys.hotkeys.find((h) => h.combo === c);
  if (hk && !(typing && !ev.ctrlKey && !ev.altKey)) { ev.preventDefault(); void runOne(hk.action); return; }
  if (typing) return;
  if (t === canvas() || (cfg.keys.captureAll && !(ev.ctrlKey && /^[a-z]$/i.test(ev.key) && !ev.altKey) && !ev.metaKey)) {
    if (forwardKey(ev)) ev.preventDefault();
  }
}

// ---------------------------------------------------------------- sequences and hotkeys

const host: Host = {
  cmd: async (l) => { await engine?.type(l); engine?.key("enter"); recordText(l, true); },
  type: async (t) => { await engine?.type(t); },
  key: (n) => engine?.key(n) ?? false,
  tap: async (r, c) => { engine?.hw(r, c, true); await new Promise((ok) => setTimeout(ok, 80)); engine?.hw(r, c, false); },
  battery: (p) => { cfg.hw.noBattery = p < 0; if (p >= 0) cfg.hw.battery = p; engine?.battery(p); syncHardware(); },
  sensor: (i, c) => { cfg.hw.sensorC[i] = c; engine?.sensor(i, c); syncHardware(); },
  gpio: (i, l) => { cfg.hw.gpioIn[i] = l; engine?.setGpio(i, l); syncHardware(); },
  shot: () => hooks.shot(),
  restart: () => restartSim(),
  note: (t) => hooks.line(`-- ${t}`, false),
};

async function runOne(line: string) {
  try { for (const s of parse(line)) await runStep(host, s, () => false); } catch (e) { hooks.banner((e as Error).message); }
}

let stopFlag = false, running = false;
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
function mergeTypes(buf: string[]): string[] {
  const out: string[] = [];
  for (const l of buf) {
    const last = out[out.length - 1];
    if (l.startsWith("type ") && last?.startsWith("type ")) out[out.length - 1] = last + l.slice(5);
    else out.push(l);
  }
  return out;
}

function fillSequences(select?: string) {
  const pick$ = $<HTMLSelectElement>("sq-pick"), text = $<HTMLTextAreaElement>("sq-text");
  const names = Object.keys(cfg.sequences);
  pick$.replaceChildren(...names.map((n) => Object.assign(document.createElement("option"), { value: n, textContent: n })));
  const pick = select && names.includes(select) ? select : names[0];
  if (pick) { pick$.value = pick; text.value = cfg.sequences[pick]; } else text.value = "";
}

function buildHotkeys() {
  $("hk-body").replaceChildren(...cfg.keys.hotkeys.map((h, i) => {
    const tr = el("tr");
    const keyBtn = el<HTMLButtonElement>("button", "btn small", h.combo || "PRESS…"); keyBtn.type = "button";
    keyBtn.onclick = () => { $("cap").hidden = false; capturing = (c) => { $("cap").hidden = true; if (c) { h.combo = c; commit(); buildHotkeys(); } }; };
    const act = Object.assign(document.createElement("input"), { type: "text", value: h.action, size: 22 }); act.autocomplete = "off"; act.spellcheck = false;
    act.oninput = () => { h.action = act.value; commit(); };
    const del = el<HTMLButtonElement>("button", "btn small rose", "✕"); del.type = "button"; del.title = "Remove";
    del.onclick = () => { cfg.keys.hotkeys.splice(i, 1); commit(); buildHotkeys(); };
    for (const c of [keyBtn, act, del]) { const td = el("td"); td.append(c); tr.append(td); }
    return tr;
  }));
}

// ---------------------------------------------------------------- settings: the segmented controls

/** Fills a .seg from its data-opts ("value:LABEL,...") and keeps the pressed one in step with get(). */
function seg(id: string, get: () => string, set: (v: string) => void, opts?: [string, string][]) {
  const root = $(id);
  const list = opts ?? (root.dataset.opts ?? "").split(",").map((p) => p.split(":") as [string, string]);
  const mark = () => { for (const b of root.querySelectorAll<HTMLButtonElement>("button")) b.setAttribute("aria-pressed", String(b.dataset.v === get())); };
  root.replaceChildren(...list.map(([v, label]) => {
    const b = el<HTMLButtonElement>("button", "", label); b.type = "button"; b.dataset.v = v;
    b.onclick = () => { set(v); mark(); };
    return b;
  }));
  mark();
  return mark;
}

// ---------------------------------------------------------------- settings: hardware

let leds: HTMLElement[] = [];
function buildHardware() {
  const h = cfg.hw;
  $i("h-bat").value = String(h.battery);
  showBattery();
  $i("h-nobat").checked = h.noBattery;
  $i("h-leds").value = String(h.leds);
  $i("h-sens").value = String(h.sensors);
  $i("h-gpios").value = String(h.gpios);
  $i("h-i2c").checked = h.i2c;
  $("h-led-card").hidden = h.leds === 0;
  $("h-sen-card").hidden = h.sensors === 0;
  $("h-gpio-card").hidden = h.gpios === 0;
  $("h-i2c-card").hidden = !h.i2c;
  leds = [];
  $("h-led").replaceChildren(...Array.from({ length: h.leds }, (_, i) => {
    const chip = el("span", "chip led"); chip.append(el("i", "dot"), el("span", "", `LED ${i + 1}`), el("code", "", "off")); leds.push(chip); return chip;
  }));
  $("leds").replaceChildren(...Array.from({ length: active() ? h.leds : 0 }, () => el("i")));
  $("h-sen").replaceChildren(...Array.from({ length: h.sensors }, (_, i) => {
    const row = el("label"); row.append(el("span", "", `Temperature ${i + 1}`));
    const r = Object.assign(document.createElement("input"), { type: "range", min: "-20", max: "90", step: "0.5", value: String(h.sensorC[i]) });
    const o = el("output", "out", `${h.sensorC[i]} °C`);
    r.oninput = () => { h.sensorC[i] = Number(r.value); o.textContent = `${r.value} °C`; engine?.sensor(i, h.sensorC[i]); commit(); };
    row.append(r, o); return row;
  }));
  $("h-gpio").replaceChildren(...Array.from({ length: h.gpios }, (_, i) => {
    const b = el<HTMLButtonElement>("button", "chip pin"); b.type = "button"; b.dataset.i = String(i); b.textContent = `PIN ${10 + i}: 0`;
    b.onclick = () => { h.gpioIn[i] = engine ? 1 - engine.gpio(i) : 1 - h.gpioIn[i]; engine?.setGpio(i, h.gpioIn[i]); commit(); };
    return b;
  }));
  buildI2c();
}

function showBattery() {
  const h = cfg.hw;
  $("h-bat-o").textContent = h.noBattery ? "NONE" : `${h.battery}%`;
  $("h-bat-fill").style.width = h.noBattery ? "0" : `${h.battery}%`;
  $("h-bat-fill").style.background = h.battery <= 15 ? "var(--rose)" : "";
}

function buildI2c() {
  $("h-i2c-list").replaceChildren(...cfg.hw.i2cAddrs.map((a) => {
    const c = el<HTMLButtonElement>("button", "chip", `0x${a.toString(16).padStart(2, "0")}  ✕`); c.type = "button"; c.title = "Remove";
    c.onclick = () => { cfg.hw.i2cAddrs = cfg.hw.i2cAddrs.filter((x) => x !== a); engine?.i2c(a, false); commit(); buildI2c(); };
    return c;
  }));
}

function syncHardware() { buildHardware(); commit(); }

// ---------------------------------------------------------------- settings: Wi-Fi networks as cards

const bars = (rssi: number) => {
  const n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
  const b = el("span", "bars"); b.title = `${rssi} dBm`;
  for (let k = 1; k <= 4; k++) { const i = el("i", k <= n ? "on" : ""); i.style.height = `${k * 4}px`; b.append(i); }
  return b;
};

function buildAps() {
  $("n-aps").replaceChildren(...cfg.wifi.aps.map((a: Ap, i) => {
    const card = el("div", "ap");
    const head = el("div", "head");
    let b = bars(a.rssi);
    const name = Object.assign(document.createElement("input"), { type: "text", value: a.ssid }); name.setAttribute("aria-label", "Network name"); name.spellcheck = false;
    name.oninput = () => { a.ssid = name.value.slice(0, 32); wifiChanged(); };
    const del = el<HTMLButtonElement>("button", "btn small rose", "✕"); del.type = "button"; del.title = "Remove this network";
    del.onclick = () => { cfg.wifi.aps.splice(i, 1); buildAps(); wifiChanged(); };
    head.append(b, name, del);
    const sig = el("div", "line"); sig.append(el("span", "", "Signal"));
    const r = Object.assign(document.createElement("input"), { type: "range", min: "-95", max: "-30", value: String(a.rssi) });
    const ro = el("span", "out", `${a.rssi} dBm`);
    r.oninput = () => { a.rssi = Number(r.value); ro.textContent = `${a.rssi} dBm`; const nb = bars(a.rssi); b.replaceWith(nb); b = nb; wifiChanged(); };
    sig.append(r, ro);
    const ch = el("div", "line"); ch.append(el("span", "", "Channel"));
    const chi = Object.assign(document.createElement("input"), { type: "number", min: "1", max: "165", value: String(a.channel) });
    chi.oninput = () => { a.channel = Math.max(1, Math.min(165, Number(chi.value) || 1)); wifiChanged(); };
    const sec = el<HTMLLabelElement>("label", "sw"); const sb = Object.assign(document.createElement("input"), { type: "checkbox", checked: a.secure });
    sec.append(sb, el("i"), el("span", "", "Secure"));
    ch.append(chi, sec);
    const pw = el("div", "line"); pw.append(el("span", "", "Password"));
    const pwi = Object.assign(document.createElement("input"), { type: "text", value: a.password, placeholder: "any" }); pwi.autocomplete = "off"; pwi.spellcheck = false;
    pwi.oninput = () => { a.password = pwi.value; wifiChanged(); };
    pw.append(pwi);
    pw.hidden = !a.secure;
    sb.onchange = () => { a.secure = sb.checked; pw.hidden = !a.secure; wifiChanged(); };
    card.append(head, sig, ch, pw);
    return card;
  }));
}
function wifiChanged() { commit(); engine?.applyWifi(cfg); }

// ---------------------------------------------------------------- live values

const RADIO = ["off", "starting", "on", "stopping", "failed"];
function poll() {
  if (!engine || !active()) return;
  for (let i = 0; i < leds.length; i++) {
    const v = engine.led(i), hex = "#" + (v & 0xffffff).toString(16).padStart(6, "0");
    (leds[i].firstElementChild as HTMLElement).style.background = v ? hex : "";
    leds[i].lastElementChild!.textContent = v ? hex : "off";
    const d = $("leds").children[i] as HTMLElement | undefined;
    if (d) { d.style.background = v ? hex : ""; d.style.boxShadow = v ? `0 0 8px 2px ${hex}` : ""; }
  }
  document.querySelectorAll<HTMLButtonElement>("#h-gpio button").forEach((b) => {
    const lv = engine!.gpio(Number(b.dataset.i)); b.textContent = `PIN ${10 + Number(b.dataset.i)}: ${lv}`; b.dataset.level = String(lv);
  });
  $("n-wifi-radio").textContent = RADIO[engine.radio(0)] ?? "?";
  $("n-ble-radio").textContent = RADIO[engine.radio(1)] ?? "?";
  $("n-wifi-link").textContent = engine.wifiConnected() ? "connected" : "not connected";
  $("p-frames").textContent = String(engine.frames);
  $("p-fps").textContent = `${fps} per second`;
  $("p-up").textContent = `${Math.round((performance.now() - booted) / 1000)} s`;
}

// ---------------------------------------------------------------- wiring the settings (works before the start too)

const RES = ["320x240", "240x240", "240x135", "160x128", "128x64"];

export function bindSettings(isActive: () => boolean, h: SimHooks) {
  active = isActive;
  hooks = h;
  addEventListener("keydown", onKey, true);
  requestAnimationFrame(loop);
  setInterval(poll, 250);
  const s = cfg.screen;

  /* screen */
  const resMark = seg("d-res-seg", () => s.res, (v) => { $i("d-res").value = v; }, RES.map((r) => [r, r.replace("x", "×")]));
  $i("d-res").value = s.res;
  $i("d-res").oninput = () => resMark();
  $("d-res-apply").onclick = () => {
    const m = /^(\d{2,3})x(\d{2,3})$/.exec($i("d-res").value.trim().replace("×", "x"));
    if (!m || +m[1] < 64 || +m[1] > 320 || +m[2] < 48 || +m[2] > 240) return hooks.banner("The screen is WIDTHxHEIGHT, from 64x48 up to 320x240");
    hooks.banner(""); s.res = `${m[1]}x${m[2]}`; commit(); resMark();
    if (engine) restartSim();
  };
  seg("d-scale", () => s.scale, (v) => { s.scale = v; commit(); applyScreen(); });
  $i("d-bright").value = String(s.brightness); $("d-bright-o").textContent = `${s.brightness}%`;
  $i("d-bright").oninput = () => { s.brightness = Number($i("d-bright").value); $("d-bright-o").textContent = `${s.brightness}%`; commit(); applyScreen(); };
  for (const [id, key] of [["d-smooth", "smooth"], ["d-grid", "grid"], ["d-gray", "grayscale"], ["d-invert", "invert"], ["d-bezel", "bezel"]] as const) {
    $i(id).checked = s[key];
    $i(id).onchange = () => { s[key] = $i(id).checked; commit(); applyScreen(); };
  }

  /* Wi-Fi */
  buildAps();
  $i("n-ip").value = cfg.wifi.ip;
  $i("n-ip").onchange = () => { if (/^\d{1,3}(\.\d{1,3}){3}$/.test($i("n-ip").value)) { cfg.wifi.ip = $i("n-ip").value; wifiChanged(); } else $i("n-ip").value = cfg.wifi.ip; };
  $i("n-refuse").checked = cfg.wifi.refuse;
  $i("n-refuse").onchange = () => { cfg.wifi.refuse = $i("n-refuse").checked; wifiChanged(); };
  $("n-drop").onclick = () => engine?.wifiDrop();
  $("n-add").onclick = () => { cfg.wifi.aps.push({ ssid: "NewNetwork", rssi: -55, channel: 6, secure: true, password: "" }); buildAps(); wifiChanged(); };
  $("n-reset").onclick = () => { cfg.wifi.aps = DEFAULT_APS.map((a) => ({ ...a })); buildAps(); wifiChanged(); };

  /* the bridge */
  const HELP: Record<string, string> = { direct: "ws://host:port: the target speaks WebSocket.", proxy: "Raw TCP through a bridge (the proxy URL below).", off: "Every connect is refused." };
  const bridgeShown = () => { $("b-proxy-row").hidden = cfg.bridge.mode !== "proxy"; $("b-mode-help").textContent = HELP[cfg.bridge.mode]; };
  seg("b-mode", () => cfg.bridge.mode, (v) => { cfg.bridge.mode = v as Config["bridge"]["mode"]; commit(); bridgeShown(); });
  bridgeShown();
  $i("b-proxy").value = cfg.bridge.proxy;
  $i("b-proxy").onchange = () => { cfg.bridge.proxy = $i("b-proxy").value; commit(); };

  /* power and hardware */
  buildHardware();
  $i("h-bat").oninput = () => { cfg.hw.battery = Number($i("h-bat").value); cfg.hw.noBattery = false; $i("h-nobat").checked = false; showBattery(); engine?.battery(cfg.hw.battery); commit(); };
  $i("h-nobat").onchange = () => { cfg.hw.noBattery = $i("h-nobat").checked; showBattery(); engine?.battery(cfg.hw.noBattery ? -1 : cfg.hw.battery); commit(); };
  $("h-apply").onclick = () => {
    const n = (id: string) => Math.max(0, Math.min(8, Math.floor(Number($i(id).value) || 0)));
    cfg.hw.leds = n("h-leds"); cfg.hw.sensors = n("h-sens"); cfg.hw.gpios = n("h-gpios"); cfg.hw.i2c = $i("h-i2c").checked;
    commit(); buildHardware();
    if (engine) restartSim();
  };
  $("h-i2c-form").onsubmit = (e) => {
    e.preventDefault();
    const v = parseInt($i("h-i2c-addr").value.trim().replace(/^0x/i, ""), 16);
    if (!(v >= 0x08 && v <= 0x77)) return hooks.banner("I2C addresses run from 0x08 to 0x77");
    hooks.banner(""); if (!cfg.hw.i2cAddrs.includes(v)) cfg.hw.i2cAddrs.push(v);
    engine?.i2c(v, true); commit(); buildI2c(); $i("h-i2c-addr").value = "";
  };

  /* input */
  $i("k-all").checked = cfg.keys.captureAll; $i("k-all").onchange = () => { cfg.keys.captureAll = $i("k-all").checked; commit(); };
  $i("k-sticky").checked = cfg.keys.sticky; $i("k-sticky").onchange = () => { cfg.keys.sticky = $i("k-sticky").checked; commit(); };
  $("k-form").onsubmit = (e) => { e.preventDefault(); const f = $i("k-line"); void host.cmd(f.value); f.value = ""; };
  buildKeyboard($("kb"), { down: (r, c) => engine?.hw(r, c, true), up: (r, c) => engine?.hw(r, c, false), sticky: () => cfg.keys.sticky });
  buildHotkeys();
  $("hk-add").onclick = () => { cfg.keys.hotkeys.push({ combo: "", action: "key enter" }); commit(); buildHotkeys(); ($("hk-body").lastElementChild!.querySelector("button") as HTMLButtonElement).click(); };
  $("hk-reset").onclick = () => { cfg.keys.hotkeys = DEFAULT_HOTKEYS.map((x: Hotkey) => ({ ...x })); commit(); buildHotkeys(); };
  const sqText = $<HTMLTextAreaElement>("sq-text"), sqPick = $<HTMLSelectElement>("sq-pick"), msg = $("sq-msg");
  fillSequences();
  sqPick.onchange = () => { sqText.value = cfg.sequences[sqPick.value] ?? ""; };
  sqText.oninput = () => { if (sqPick.value) { cfg.sequences[sqPick.value] = sqText.value; commit(); } };
  $("sq-new").onclick = () => {
    let n = 1; while (cfg.sequences[`Sequence ${n}`]) n++;
    cfg.sequences[`Sequence ${n}`] = "# steps, one per line\ncmd help"; commit(); fillSequences(`Sequence ${n}`);
  };
  $("sq-del").onclick = () => { if (sqPick.value) { delete cfg.sequences[sqPick.value]; commit(); fillSequences(); } };
  $("sq-run").onclick = async () => {
    if (running) return;
    if (!engine) { msg.textContent = "start the simulator first"; return; }
    running = true; stopFlag = false; msg.textContent = ""; hooks.banner("");
    ($("sq-run") as HTMLButtonElement).disabled = true; ($("sq-stop") as HTMLButtonElement).disabled = false;
    try { await runScript(host, sqText.value, Math.max(1, Number($i("sq-rep").value) || 1), () => stopFlag, (st) => (msg.textContent = `line ${st.line}: ${st.verb}`)); msg.textContent = stopFlag ? "stopped" : "done"; }
    catch (e) { msg.textContent = (e as Error).message; }
    running = false; ($("sq-run") as HTMLButtonElement).disabled = false; ($("sq-stop") as HTMLButtonElement).disabled = true;
  };
  $("sq-stop").onclick = () => { stopFlag = true; };
  $("sq-rec").onclick = () => {
    recording = !recording;
    $("sq-rec").setAttribute("aria-pressed", String(recording)); $("sq-rec").textContent = recording ? "STOP RECORDING" : "RECORD";
    if (recording) { recBuf = []; recLast = 0; canvas().focus(); }
    else if (recBuf.length) { sqText.value = (sqText.value.trimEnd() ? sqText.value.trimEnd() + "\n" : "") + mergeTypes(recBuf).join("\n"); sqText.dispatchEvent(new Event("input")); }
  };

  $("p-iso").textContent = crossOriginIsolated ? "yes" : "no";
  applyScreen();
}

// ---------------------------------------------------------------- start, restart, stop

/** Starts the system. A page without isolation registers the service worker that adds it and reloads once (returns null). */
export async function startSim(): Promise<Engine | null> {
  if (!crossOriginIsolated) {
    if ("serviceWorker" in navigator && isSecureContext && location.protocol !== "file:" && !sessionStorage.getItem("palanu.sim.coi")) {
      sessionStorage.setItem("palanu.sim.coi", "1");
      startAfterReload();
      hooks.stage("Preparing");
      await navigator.serviceWorker.register("coi-sw.js");
      await navigator.serviceWorker.ready;
      location.reload();
      return null;
    }
    throw new Error(simBlocker() || "The page is not cross-origin isolated: start it with ./pal web.");
  }
  installBridge(() => cfg, (s) => hooks.line(`[net] ${s}`, false));
  booted = performance.now();
  fpsAt = booted;
  const e = await Engine.boot(cfg, hooks.line, hooks.stage);
  engine = e;
  e.applyWifi(cfg);
  for (const a of [0x48, 0x68]) if (!cfg.hw.i2cAddrs.includes(a)) e.i2c(a, false);
  for (const a of cfg.hw.i2cAddrs) e.i2c(a, true);
  cfg.hw.sensorC.forEach((c, i) => i < cfg.hw.sensors && e.sensor(i, c));
  cfg.hw.gpioIn.forEach((l, i) => i < cfg.hw.gpios && e.setGpio(i, l));
  const c = canvas();
  c.width = e.w; c.height = e.h;
  c.style.aspectRatio = `${e.w} / ${e.h}`;
  $("pixgrid").style.backgroundSize = `calc(100% / ${e.w}) calc(100% / ${e.h})`;
  buildHardware();
  applyScreen();
  return e;
}

/** The system restarts with what it saved (and the settings read at start: screen, devices). */
export function restartSim() { engine?.persist(); startAfterReload(); setTimeout(() => location.reload(), 150); }
/** The system stops: its files are saved and the page comes back without it. */
export function stopSim() { engine?.persist(); setTimeout(() => location.reload(), 150); }
export async function eraseSim() { await Engine.eraseData(); location.reload(); }

/** The virtual cable to the system's Palanu Link (platform/web/web_main.c): the page polls the device's bytes each 8 ms. */
export function simTransport(e: Engine): Transport {
  const M = e.M, CAP = 16384;
  const rbuf: number = M._malloc(CAP), wbuf: number = M._malloc(CAP);
  const read = () => e.c("pal_web_link_read", "number", ["number", "number"], [rbuf, CAP]) as number;
  while (read() > 0) { /* drain what a last connection left */ }
  e.c("pal_web_link_reset", null, [], []);
  let cb: (d: Uint8Array) => void = () => {};
  let closed: () => void = () => {};
  const timer = setInterval(() => {
    for (let n = read(); n > 0; n = read()) cb(M.HEAPU8.slice(rbuf, rbuf + n));
  }, 8);
  return {
    kind: "Virtual USB", tag: 1,
    write: async (d) => {
      for (let off = 0; off < d.length;) {
        const part = d.subarray(off, off + CAP);
        M.HEAPU8.set(part, wbuf);
        const k = e.c("pal_web_link_write", "number", ["number", "number"], [wbuf, part.length]) as number;
        off += k;
        if (k < part.length) await new Promise((ok) => setTimeout(ok, 4)); /* the ring is full: the system catches up */
      }
    },
    onData: (f) => (cb = f),
    onClose: (f) => (closed = f),
    close: async () => { clearInterval(timer); closed(); },
  };
}

export function simFps() { return fps; }
export function simUptime() { return booted ? performance.now() - booted : 0; }
