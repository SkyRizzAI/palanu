// The PalanuOS WebAssembly build (palanu-web.js + .wasm, from platform/web) behind a small typed wrapper. The OS runs on
// Web Workers (one per Nema task) in this page, so the page must be cross-origin isolated (forge/web/serve.ts sets the headers).
import type { Config } from "./config.ts";

/* eslint-disable @typescript-eslint/no-explicit-any */
export class Engine {
  M: any;
  private frameSeen = -1;
  w = 320;
  h = 240;
  private img = new ImageData(320, 240);
  onLine: (text: string, err: boolean) => void = () => {};

  /** Loads the module, mounts the persistent /data, and starts the OS with the boot-time hardware options. */
  static async boot(cfg: Config, onLine: (t: string, err: boolean) => void, onStage: (s: string) => void): Promise<Engine> {
    const e = new Engine();
    e.onLine = onLine;
    onStage("Loading the system");
    await new Promise<void>((ok, bad) => {
      const s = document.createElement("script");
      s.src = "palanu-web.js";
      s.onload = () => ok();
      s.onerror = () => bad(new Error("palanu-web.js not found: build it with ./pal web (needs the Emscripten build, see docs/SIMULATOR.md)"));
      document.head.append(s);
    });
    e.M = await (window as any).PalanuModule({
      print: (t: string) => e.onLine(t, false),
      printErr: (t: string) => e.onLine(t, true),
      locateFile: (p: string) => p,
      noInitialRun: true,
      preRun: [(M: any) => {
        M.FS.mkdir("/data");
        M.FS.mount(M.IDBFS, {}, "/data");
        M.addRunDependency("idbfs");
        M.FS.syncfs(true, () => M.removeRunDependency("idbfs"));
      }],
    });
    onStage("Starting");
    const h = cfg.hw;
    const args = ["--screen", cfg.screen.res, "--battery", String(h.noBattery ? -1 : h.battery), "--leds", String(h.leds), "--sensors", String(h.sensors), "--gpios", String(h.gpios)];
    if (h.i2c) args.push("--i2c");
    e.M.callMain(args);
    /* callMain returns before main has run on its worker, so the size is the one asked for, not read back */
    const m = /^(\d+)x(\d+)$/.exec(cfg.screen.res);
    e.w = m ? Number(m[1]) : 320;
    e.h = m ? Number(m[2]) : 240;
    e.img = new ImageData(e.w, e.h);
    setInterval(() => e.persist(), 3000);
    addEventListener("pagehide", () => e.persist());
    return e;
  }

  static erasing = false;
  persist() { if (Engine.erasing) return; try { this.M.FS.syncfs(false, () => {}); } catch { /* not ready */ } }

  c(name: string, ret: "number" | "string" | null, types: ("number" | "string")[], args: unknown[]): any {
    return this.M.ccall(name, ret, types, args);
  }

  /** The newest frame as ImageData, or null when the screen has not changed since the last call. */
  frame(): ImageData | null {
    const n = this.c("pal_web_frame_counter", "number", [], []);
    if (n === this.frameSeen) return null;
    this.frameSeen = n;
    const p = this.c("pal_web_frame", "number", [], []);
    this.img.data.set(this.M.HEAPU8.subarray(p, p + this.w * this.h * 4));
    return this.img;
  }
  get frames() { return this.frameSeen; }

  key(name: string): boolean { return this.c("pal_web_key_named", "number", ["string"], [name]) === 0; }
  keySym(sym: number, cp: number, mods: number) { this.c("pal_web_key", null, ["number", "number", "number"], [sym, cp, mods]); }
  /** Types text a few characters at a time: the OS reads keys from a small queue, and a long line pushed at once overflows it
   *  (the native simulator pauses 4 ms between characters for the same reason). */
  async type(text: string) {
    const chars = [...text];
    for (let i = 0; i < chars.length; i += 4) {
      this.c("pal_web_type", null, ["string"], [chars.slice(i, i + 4).join("")]);
      await new Promise((ok) => setTimeout(ok, 12));
    }
  }
  hw(row: number, col: number, down: boolean) { this.c("pal_web_hw", null, ["number", "number", "number"], [row, col, down ? 1 : 0]); }

  battery(pct: number) { this.c("pal_web_set_battery", null, ["number"], [pct]); }
  led(i: number): number { return this.c("pal_web_led", "number", ["number"], [i]); }
  sensor(i: number, celsius: number) { this.c("pal_web_sensor_set", null, ["number", "number"], [i, Math.round(celsius * 1000)]); }
  gpio(i: number): number { return this.c("pal_web_gpio_get", "number", ["number"], [i]); }
  setGpio(i: number, level: number) { this.c("pal_web_gpio_set", null, ["number", "number"], [i, level]); }
  i2c(addr: number, present: boolean) { this.c("pal_web_i2c_set", null, ["number", "number"], [addr, present ? 1 : 0]); }
  radio(r: number): number { return this.c("pal_web_radio_state", "number", ["number"], [r]); }

  applyWifi(cfg: Config) {
    this.c("pal_web_ap_clear", null, [], []);
    for (const a of cfg.wifi.aps) this.c("pal_web_ap_add", "number", ["string", "number", "number", "number", "string"], [a.ssid, a.rssi, a.channel, a.secure ? 1 : 0, a.password]);
    this.c("pal_web_wifi_config", null, ["string", "number"], [cfg.wifi.ip, cfg.wifi.refuse ? 1 : 0]);
  }
  wifiConnected(): boolean { return this.c("pal_web_wifi_connected", "number", [], []) === 1; }
  wifiDrop() { this.c("pal_web_wifi_drop", null, [], []); }

  /** Erases the saved /data (the next start is a first boot). */
  static eraseData(): Promise<void> {
    Engine.erasing = true;
    return new Promise((ok) => { const r = indexedDB.deleteDatabase("/data"); r.onsuccess = r.onerror = r.onblocked = () => ok(); });
  }
}

/** The network bridge: the OS opens sockets, the browser can only open WebSockets, so each connect to host:port becomes
 *  a WebSocket to ws://host:port (direct: the target speaks WebSocket) or to a proxy that forwards raw TCP (proxy). */
export function installBridge(cfg: () => Config, note: (s: string) => void) {
  const Native = window.WebSocket;
  const Patched = function (this: unknown, url: string | URL, protocols?: string | string[]) {
    const b = cfg().bridge;
    let u = String(url);
    if (b.mode === "off") { note(`bridge off: ${u} refused`); throw new DOMException("network bridge is off", "NetworkError"); }
    if (b.mode === "proxy") {
      const m = /^wss?:\/\/([^:/]+):(\d+)/.exec(u);
      if (m) u = b.proxy.replace("{host}", m[1]).replace("{port}", m[2]);
    }
    note(`bridge ${b.mode}: ${String(url)}${u !== String(url) ? " -> " + u : ""}`);
    return new Native(u, protocols);
  } as unknown as typeof WebSocket;
  Patched.prototype = Native.prototype;
  for (const k of ["CONNECTING", "OPEN", "CLOSING", "CLOSED"] as const) (Patched as any)[k] = (Native as any)[k];
  window.WebSocket = Patched;
}
