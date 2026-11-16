// The three ways a browser reaches a PalanuOS device, all straight from the page with no server in between:
//   USB        Web Serial: the board's USB-UART (console and Palanu Link share it). Chrome and Edge on a computer.
//   Bluetooth  Web Bluetooth: the Palanu Link GATT service. Chrome and Edge (computer, Android).
//   Wi-Fi/LAN  WebSocket to the device's port 7001 (system/src/link_ws.c). Every browser; the page must not be https.
import type { Transport } from "./link.ts";

// docs/PLP.md, "Transport": the GATT service and its two characteristics.
export const BLE_SERVICE = "6f1d0001-3b7a-4c1f-8f8b-52d1371c9a4e";
const BLE_RX = "6f1d0002-3b7a-4c1f-8f8b-52d1371c9a4e"; /* we write */
const BLE_TX = "6f1d0003-3b7a-4c1f-8f8b-52d1371c9a4e"; /* the device notifies */
const BLE_CHUNK = 180; /* below the usual negotiated MTU; Web Bluetooth does not expose it */
export const WS_PORT = 7001;

// Web Serial and Web Bluetooth are not in TypeScript's DOM types: the parts used here.
type SerialPort = {
  open(o: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  setSignals?(s: { dataTerminalReady?: boolean; requestToSend?: boolean }): Promise<void>;
};
type GattChar = {
  writeValueWithoutResponse?(v: BufferSource): Promise<void>;
  writeValue(v: BufferSource): Promise<void>;
  startNotifications(): Promise<GattChar>;
  addEventListener(t: string, f: (e: Event) => void): void;
  value?: DataView;
};

const nav = navigator as Navigator & {
  serial?: { requestPort(o?: object): Promise<SerialPort> };
  bluetooth?: { requestDevice(o: object): Promise<any> };
};

export const support = {
  usb: !!nav.serial,
  ble: !!nav.bluetooth,
  ws: typeof WebSocket !== "undefined",
  /** an https page may not open ws:// to the device (mixed content) */
  wsBlocked: location.protocol === "https:",
};

export async function connectUsb(): Promise<Transport> {
  if (!nav.serial) throw new Error("this browser has no Web Serial: use Chrome or Edge on a computer");
  const port = await nav.serial.requestPort();
  await port.open({ baudRate: 115200 });
  let cb: (d: Uint8Array) => void = () => {};
  let closed: () => void = () => {};
  let open = true;
  const reader = port.readable!.getReader();
  const writer = port.writable!.getWriter();
  (async () => {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) cb(value);
      }
    } catch { /* the cable was pulled */ }
    if (open) { open = false; closed(); }
  })();
  return {
    kind: "USB", tag: 1,
    write: (d) => writer.write(d),
    onData: (f) => (cb = f),
    onClose: (f) => (closed = f),
    close: async () => {
      open = false;
      try { await reader.cancel(); } catch { /* already gone */ }
      reader.releaseLock();
      writer.releaseLock();
      try { await port.close(); } catch { /* already gone */ }
    },
  };
}

export async function connectBle(): Promise<Transport> {
  if (!nav.bluetooth) throw new Error("this browser has no Web Bluetooth: use Chrome or Edge");
  const dev = await nav.bluetooth.requestDevice({ filters: [{ services: [BLE_SERVICE] }, { namePrefix: "PalanuOS" }], optionalServices: [BLE_SERVICE] });
  const server = await dev.gatt.connect();
  const svc = await server.getPrimaryService(BLE_SERVICE);
  const rx: GattChar = await svc.getCharacteristic(BLE_RX);
  const tx: GattChar = await svc.getCharacteristic(BLE_TX);
  let cb: (d: Uint8Array) => void = () => {};
  let closed: () => void = () => {};
  tx.addEventListener("characteristicvaluechanged", (e) => {
    const v = (e.target as unknown as GattChar).value!;
    cb(new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice());
  });
  await tx.startNotifications(); /* before HELLO (docs/PLP.md) */
  dev.addEventListener("gattserverdisconnected", () => closed());
  return {
    kind: "Bluetooth", tag: 3,
    write: async (d) => {
      for (let i = 0; i < d.length; i += BLE_CHUNK) {
        const part = d.slice(i, i + BLE_CHUNK);
        if (rx.writeValueWithoutResponse) await rx.writeValueWithoutResponse(part);
        else await rx.writeValue(part);
      }
    },
    onData: (f) => (cb = f),
    onClose: (f) => (closed = f),
    close: async () => { try { dev.gatt.disconnect(); } catch { /* already gone */ } },
  };
}

/** addr: "192.168.1.3", "192.168.1.3:7001" or "ws://..." */
export function connectWs(addr: string): Promise<Transport> {
  let url = addr.trim();
  if (!/^wss?:\/\//.test(url)) url = "ws://" + (/:\d+$/.test(url) ? url : `${url}:${WS_PORT}`);
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    let cb: (d: Uint8Array) => void = () => {};
    let closed: () => void = () => {};
    const timer = setTimeout(() => { ws.close(); reject(new Error(`no WebSocket at ${url} (is Wi-Fi on and the device on this network?)`)); }, 6000);
    ws.onopen = () => {
      clearTimeout(timer);
      resolve({
        kind: "Wi-Fi/LAN", tag: 4,
        write: async (d) => { if (ws.readyState === WebSocket.OPEN) ws.send(d as Uint8Array<ArrayBuffer>); },
        onData: (f) => (cb = f),
        onClose: (f) => (closed = f),
        close: async () => ws.close(),
      });
    };
    ws.onmessage = (e) => cb(new Uint8Array(e.data as ArrayBuffer));
    ws.onerror = () => { clearTimeout(timer); reject(new Error(`cannot open ${url}`)); };
    ws.onclose = () => closed();
  });
}
