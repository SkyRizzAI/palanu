// The simulator's settings: what the page remembers between visits (localStorage, this browser only).
export interface Ap { ssid: string; rssi: number; channel: number; secure: boolean; password: string }
export interface Hotkey { combo: string; action: string }
export interface Config {
  screen: { res: string; scale: string; smooth: boolean; bezel: boolean; brightness: number; grayscale: boolean; invert: boolean; grid: boolean };
  hw: { leds: number; sensors: number; gpios: number; i2c: boolean; battery: number; noBattery: boolean; sensorC: number[]; gpioIn: number[]; i2cAddrs: number[] };
  wifi: { aps: Ap[]; ip: string; refuse: boolean };
  bridge: { mode: "off" | "direct" | "proxy"; proxy: string };
  keys: { captureAll: boolean; sticky: boolean; hotkeys: Hotkey[] };
  log: { max: number; wrap: boolean };
  sequences: Record<string, string>;
}

export const DEFAULT_APS: Ap[] = [
  { ssid: "MyHomeWiFi", rssi: -42, channel: 36, secure: true, password: "" },
  { ssid: "CoffeeShop_Free", rssi: -68, channel: 6, secure: false, password: "" },
  { ssid: "Neighbour_5G", rssi: -74, channel: 11, secure: true, password: "" },
  { ssid: "AndroidAP", rssi: -81, channel: 1, secure: true, password: "" },
];

export const DEFAULT_HOTKEYS: Hotkey[] = [
  { combo: "Ctrl+Shift+S", action: "shot" },
  { combo: "Ctrl+Shift+R", action: "restart" },
  { combo: "Alt+H", action: "key home-app" },
  { combo: "Alt+W", action: "key switch" },
  { combo: "Alt+K", action: "key vkb" },
];

export function defaults(): Config {
  return {
    screen: { res: "320x240", scale: "fit", smooth: false, bezel: true, brightness: 100, grayscale: false, invert: false, grid: false },
    hw: { leds: 1, sensors: 1, gpios: 2, i2c: true, battery: 80, noBattery: false, sensorC: [21.5, 22.5, 23.5, 24.5, 25.5, 26.5, 27.5, 28.5], gpioIn: [0, 0, 0, 0, 0, 0, 0, 0], i2cAddrs: [0x48, 0x68] },
    wifi: { aps: DEFAULT_APS.map((a) => ({ ...a })), ip: "192.168.1.20", refuse: false },
    bridge: { mode: "direct", proxy: "ws://" + (location.host || "localhost:5174") + "/tcp/{host}/{port}" },
    keys: { captureAll: false, sticky: true, hotkeys: DEFAULT_HOTKEYS.map((h) => ({ ...h })) },
    log: { max: 5000, wrap: true },
    sequences: { "Open terminal": "# home, the launcher, its first app (the terminal)\nkey home-app\nwait 500\nkey enter\nwait 700\nkey left left left left left left\nwait 300\nkey enter\nwait 800\ncmd help", "Hello": "cmd echo hello from the browser" },
  };
}

const KEY = "palanu.sim.config";

function merge<T>(base: T, over: unknown): T {
  if (over === null || typeof over !== "object" || Array.isArray(base) || typeof base !== "object") return (over === undefined || (typeof over !== typeof base)) ? base : (over as T);
  const out: Record<string, unknown> = { ...(base as Record<string, unknown>) };
  for (const k of Object.keys(over as Record<string, unknown>)) {
    const b = (base as Record<string, unknown>)[k], o = (over as Record<string, unknown>)[k];
    out[k] = b === undefined ? o : Array.isArray(b) ? (Array.isArray(o) ? o : b) : merge(b, o);
  }
  return out as T;
}

export function load(): Config {
  try {
    const raw = localStorage.getItem(KEY);
    if (raw) return merge(defaults(), JSON.parse(raw));
  } catch { /* private window or bad JSON: start from the defaults */ }
  return defaults();
}

export function save(c: Config) {
  try { localStorage.setItem(KEY, JSON.stringify(c)); } catch { /* private window */ }
}
