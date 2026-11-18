// Palanu Link client for the browser (docs/PLP.md). The same protocol as the command-line client
// (forge/packages/link/src/index.ts), with WebCrypto and without Node: hello, pairing, auth, shell, screen, input, files, OTA.
import { CH, Decoder, encodeFrame, type Frame } from "../../packages/link/src/frame.ts";
import { authResponse, concat, fromHex, openToken, pairKeys, randomBytes, toHex, x25519, x25519Keypair } from "./crypto.ts";

export { CH };

export interface Transport {
  /** "USB", "Bluetooth", "Wi-Fi/LAN": shown in the status bar. */
  kind: string;
  /** The device's transport tag for AUTH (docs/PLP.md): 1 cable, 3 BLE, 4 WebSocket. */
  tag: number;
  write(data: Uint8Array): Promise<void>;
  onData(cb: (d: Uint8Array) => void): void;
  onClose(cb: () => void): void;
  close(): Promise<void>;
}

export const PAIR = { request: 5, challenge: 6, proof: 7, ok: 8 } as const;
export const AUTH = { hello: 9, challenge: 10, response: 11, ok: 12 } as const;
export const LINK_ERR = { auth: -23, busy: -4, full: -15, state: -16, pairClosed: -27 } as const;

export class LinkError extends Error {
  constructor(public code: number, msg: string) { super(msg); }
}

export type DeviceInfo = Record<string, unknown> & { id?: string; name?: string; trusted?: boolean };
export type Entry = { name: string; dir: boolean; size: number };

const te = new TextEncoder(), td = new TextDecoder();
const u16 = (v: number) => [v & 255, (v >> 8) & 255];
const i8 = (b: number) => (b << 24) >> 24;

export class Link {
  private waiters: { match: (f: Frame) => boolean; resolve: (f: Frame) => void }[] = [];
  private dec: Decoder;
  private nextId = 1;
  private queue: Promise<void> = Promise.resolve();
  onTty: (d: Uint8Array) => void = () => {};
  onTtyClose: () => void = () => {};
  onConsole: (d: Uint8Array) => void = () => {};
  onScreen: (dirty: { x: number; y: number; w: number; h: number }) => void = () => {};
  /** rgb holds RGB565 pixels when the device sends them (SUBSCRIBE 2, RECT565); else px indexes pal. */
  screen = { w: 0, h: 0, px: new Uint8Array(0), pal: new Uint16Array(256), rgb: new Uint16Array(0), is565: false };

  constructor(public t: Transport) {
    this.dec = new Decoder((f) => this.dispatch(f), (d) => this.onConsole(d));
    t.onData((d) => this.dec.push(d));
  }

  private dispatch(f: Frame) {
    if (f.chan === CH.tty) {
      if (f.type === 2) this.onTty(f.payload);
      else if (f.type === 4) this.onTtyClose();
    } else if (f.chan === CH.screen) this.screenFrame(f);
    const i = this.waiters.findIndex((w) => w.match(f));
    if (i >= 0) this.waiters.splice(i, 1)[0].resolve(f);
  }

  private screenFrame(f: Frame) {
    const p = f.payload, s = this.screen, dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    if (f.type === 5) {
      s.w = dv.getUint16(0, true); s.h = dv.getUint16(2, true); s.px = new Uint8Array(s.w * s.h); s.rgb = new Uint16Array(s.w * s.h);
    } else if (f.type === 2) {
      const first = dv.getUint16(0, true), n = dv.getUint16(2, true);
      for (let i = 0; i < n; i++) s.pal[first + i] = dv.getUint16(4 + 2 * i, true);
      this.onScreen({ x: 0, y: 0, w: s.w, h: s.h });
    } else if (f.type === 3 && s.w) {
      const x = dv.getUint16(0, true), y = dv.getUint16(2, true), w = dv.getUint16(4, true), rows = dv.getUint16(6, true);
      let o = 8;
      for (let r = 0; r < rows && o + 2 <= p.length; r++) {
        const n = dv.getUint16(o, true); o += 2;
        let i = o, col = 0;
        const end = o + n, base = (y + r) * s.w + x;
        while (i < end && col < w) { /* PackBits */
          const c = i8(p[i++]);
          if (c >= 0) for (let k = 0; k <= c && col < w; k++) s.px[base + col++] = p[i++];
          else { const v = p[i++]; for (let k = 0; k < 1 - c && col < w; k++) s.px[base + col++] = v; }
        }
        o = end;
      }
      s.is565 = false;
      this.onScreen({ x, y, w, h: rows });
    } else if (f.type === 6 && s.w) { /* RGB565 rows, PackBits over 16-bit pixels */
      const x = dv.getUint16(0, true), y = dv.getUint16(2, true), w = dv.getUint16(4, true), rows = dv.getUint16(6, true);
      let o = 8;
      for (let r = 0; r < rows && o + 2 <= p.length; r++) {
        const n = dv.getUint16(o, true); o += 2;
        let i = o, col = 0;
        const end = o + n, base = (y + r) * s.w + x;
        while (i < end && col < w) {
          const c = i8(p[i++]);
          if (c >= 0) for (let k = 0; k <= c && col < w; k++, i += 2) s.rgb[base + col++] = p[i] | (p[i + 1] << 8);
          else { const v = p[i] | (p[i + 1] << 8); i += 2; for (let k = 0; k < 1 - c && col < w; k++) s.rgb[base + col++] = v; }
        }
        o = end;
      }
      s.is565 = true;
      this.onScreen({ x, y, w, h: rows });
    }
  }

  /** Frames go out one after another: BLE in particular must not interleave two writes. */
  send(chan: number, type: number, payload?: Uint8Array): Promise<void> {
    const f = encodeFrame(chan, type, payload);
    this.queue = this.queue.then(() => this.t.write(f)).catch(() => {});
    return this.queue;
  }

  wait(match: (f: Frame) => boolean, ms = 4000): Promise<Frame> {
    return new Promise((resolve, reject) => {
      const w = { match, resolve: (f: Frame) => { clearTimeout(timer); resolve(f); } };
      const timer = setTimeout(() => { this.waiters = this.waiters.filter((x) => x !== w); reject(new Error("the device did not answer")); }, ms);
      this.waiters.push(w);
    });
  }

  /** HELLO until INFO comes: opening a serial port resets the board, so the first ones may go unanswered. */
  async hello(retries = 15): Promise<DeviceInfo> {
    for (let i = 0; i < retries; i++) {
      const p = this.wait((f) => f.chan === CH.ctl && f.type === 2, 1000);
      void this.send(CH.ctl, 1, te.encode("web-forge"));
      try { return JSON.parse(td.decode((await p).payload)); } catch { /* retry */ }
    }
    throw new Error("no answer from the device");
  }

  ctlReply(types: number[], ms = 6000): Promise<Frame> {
    return this.wait((f) => f.chan === CH.ctl && (types.includes(f.type) || f.type === 0x7f), ms).then((f) => {
      if (f.type === 0x7f) throw new LinkError(i8(f.payload[0]), td.decode(f.payload.subarray(1)));
      return f;
    });
  }

  /** Pairs with the 6-digit code the device shows. Returns the client id and token (hex) to keep. */
  async pair(code: string, name: string): Promise<{ clientId: string; token: string }> {
    if (!/^\d{6}$/.test(code)) throw new Error("the pairing code is 6 digits");
    const c = await x25519Keypair();
    const cn = randomBytes(16);
    const nb = te.encode(name).subarray(0, 24);
    const challenge = this.ctlReply([PAIR.challenge]);
    void this.send(CH.ctl, PAIR.request, concat(Uint8Array.of(nb.length), nb, c.pub, cn));
    const f = await challenge;
    const dpub = f.payload.slice(0, 32), dn = f.payload.slice(32, 48);
    const shared = await x25519(c.priv, dpub);
    const { proof, kEnc } = await pairKeys(shared, code, concat(c.pub, dpub, cn, dn));
    const ok = this.ctlReply([PAIR.ok]);
    void this.send(CH.ctl, PAIR.proof, proof);
    const g = await ok;
    const id = g.payload.slice(0, 8), nonce = g.payload.slice(8, 20);
    const token = openToken(kEnc, id, nonce, g.payload.slice(20, 52), g.payload.slice(52, 68));
    return { clientId: toHex(id), token: toHex(token) };
  }

  async auth(clientId: string, token: string): Promise<void> {
    const id = fromHex(clientId), tok = fromHex(token);
    const challenge = this.ctlReply([AUTH.challenge]);
    void this.send(CH.ctl, AUTH.hello, id);
    const ch = await challenge;
    const ok = this.ctlReply([AUTH.ok]);
    void this.send(CH.ctl, AUTH.response, await authResponse(tok, ch.payload.slice(0, 16), id, this.t.tag));
    await ok;
  }

  async ping(): Promise<number> {
    const t0 = performance.now();
    const p = this.wait((f) => f.chan === CH.ctl && f.type === 4);
    void this.send(CH.ctl, 3, Uint8Array.of(1, 2, 3, 4));
    await p;
    return performance.now() - t0;
  }

  // ---- shell ----
  openShell(cols: number, rows: number) { return this.send(CH.tty, 1, Uint8Array.from([...u16(cols), ...u16(rows)])); }
  resize(cols: number, rows: number) { return this.send(CH.tty, 3, Uint8Array.from([...u16(cols), ...u16(rows)])); }
  ttyWrite(d: Uint8Array) {
    for (let i = 0; i < d.length; i += 512) void this.send(CH.tty, 2, d.subarray(i, i + 512));
  }
  closeShell() { return this.send(CH.tty, 4); }

  // ---- screen and keys ----
  subscribeScreen(on: boolean) { return this.send(CH.screen, 1, Uint8Array.of(on ? 2 : 0)); } // 2: full colour (RGB565) when the device has a screen frame
  refreshScreen() { return this.send(CH.screen, 4); }
  key(sym: number, cp = 0, mods = 0) {
    return this.send(CH.input, 1, Uint8Array.from([sym, mods, cp & 255, (cp >> 8) & 255, (cp >> 16) & 255, cp >>> 24]));
  }

  // ---- files ----
  private fsErr(f: Frame) { return new Error(`device error ${i8(f.payload[1])}: ${td.decode(f.payload.subarray(2))}`); }

  list(path: string): Promise<Entry[]> {
    const id = this.nextId++ & 255, out: Entry[] = [];
    return new Promise((resolve, reject) => {
      let timer = setTimeout(() => reject(new Error("timeout")), 8000);
      const w = { match: (f: Frame) => f.chan === CH.fs && f.payload[0] === id, resolve: (f: Frame) => {
        clearTimeout(timer);
        if (f.type === 2) {
          const dv = new DataView(f.payload.buffer, f.payload.byteOffset, f.payload.byteLength);
          out.push({ dir: f.payload[1] === 2, size: dv.getUint32(2, true), name: td.decode(f.payload.subarray(6)) });
          timer = setTimeout(() => reject(new Error("timeout")), 8000);
          this.waiters.push(w);
        } else if (f.type === 3) resolve(out);
        else reject(this.fsErr(f));
      } };
      this.waiters.push(w);
      void this.send(CH.fs, 1, concat(Uint8Array.of(id), te.encode(path)));
    });
  }

  read(path: string, progress: (n: number) => void = () => {}): Promise<Uint8Array> {
    const id = this.nextId++ & 255, chunks: Uint8Array[] = [];
    let got = 0;
    return new Promise((resolve, reject) => {
      let timer = setTimeout(() => reject(new Error("timeout")), 8000);
      const w = { match: (f: Frame) => f.chan === CH.fs && f.payload[0] === id, resolve: (f: Frame) => {
        clearTimeout(timer);
        if (f.type === 5) {
          chunks.push(f.payload.slice(1)); got += f.payload.length - 1; progress(got);
          timer = setTimeout(() => reject(new Error("timeout")), 8000);
          this.waiters.push(w);
        } else if (f.type === 3) resolve(concat(...chunks));
        else reject(this.fsErr(f));
      } };
      this.waiters.push(w);
      void this.send(CH.fs, 4, concat(Uint8Array.of(id), te.encode(path)));
    });
  }

  async write(path: string, data: Uint8Array, progress: (n: number) => void = () => {}): Promise<void> {
    const id = this.nextId++ & 255;
    const step = async (type: number, payload: Uint8Array) => {
      const p = this.wait((x) => x.chan === CH.fs && x.payload[0] === id, 8000);
      void this.send(CH.fs, type, payload);
      const f = await p;
      if (f.type !== 7) throw this.fsErr(f);
    };
    await step(6, concat(Uint8Array.of(id), te.encode(path)));
    for (let i = 0; i < data.length; i += 512) {
      await step(5, concat(Uint8Array.of(id), data.subarray(i, i + 512)));
      progress(Math.min(i + 512, data.length));
    }
    await step(3, Uint8Array.of(id));
  }

  /** A firmware image: the device writes it to the other slot, checks it, and restarts into it. */
  async ota(image: Uint8Array, progress: (done: number, total: number) => void = () => {}): Promise<void> {
    const step = async (type: number, payload: Uint8Array, ms = 8000) => {
      const p = this.wait((f) => f.chan === CH.ota, ms);
      void this.send(CH.ota, type, payload);
      const f = await p;
      if (f.type !== 4) throw new Error(`the device refused the update (code ${i8(f.payload[0])})`);
    };
    const n = image.length;
    await step(1, Uint8Array.from([n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255]), 30000);
    for (let i = 0; i < n; i += 512) {
      await step(2, image.subarray(i, i + 512));
      if ((i / 512) % 8 === 0) progress(i, n);
    }
    progress(n, n);
    await step(3, new Uint8Array(0), 40000);
  }

  close() { return this.t.close(); }
}
