// Palanu Link client. Wire format: docs/PLP.md.
import { spawnSync } from "node:child_process";
import { closeSync, openSync, writeSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { loadPairing } from "./tokens.ts";
import { AUTH, LINK_ERR, PAIR, authResponse, fromHex, openToken, pairKeys, randomBytes, toHex, x25519, x25519Keypair } from "./auth.ts";

export { CH, crc16, encodeFrame, Decoder } from "./frame.ts";
export type { Frame } from "./frame.ts";
import { CH, encodeFrame, Decoder, type Frame } from "./frame.ts";

export interface Transport {
  write(data: Uint8Array): void;
  onData(cb: (d: Uint8Array) => void): void;
  close(): void;
  /** The device's transport tag for AUTH (docs/PLP.md): 1 cable, 2 TCP, 3 BLE. */
  tag?: number;
}

/** A device error reply (an ERROR frame): code is the NEMA_ERR_* or PAL_LINK_E_* value. */
export class LinkError extends Error {
  constructor(public code: number, msg: string) {
    super(msg);
  }
}

export async function connectTcp(host: string, port: number): Promise<Transport> {
  let cb: (d: Uint8Array) => void = () => {};
  const sock = await Bun.connect({
    hostname: host, port,
    socket: { data(_s, d) { cb(new Uint8Array(d)); }, error() {}, close() {} },
  });
  return { write: (d) => { sock.write(d); }, onData: (f) => (cb = f), close: () => { sock.flush(); sock.terminate(); }, tag: 2 };
}

/** A serial port: configured with stty (raw, no flow control). Reading goes through `cat` because Bun's file
 * streams deliver nothing for a tty; writing uses a plain descriptor. Opening the port resets most ESP boards
 * (DTR/RTS), so `hello` keeps retrying until the firmware is up. */
export function connectSerial(path: string, baud = 115200): Transport {
  const fd = openSync(path, "r+"); // keep it open: termios settings last only while a descriptor is open
  const flag = process.platform === "darwin" ? "-f" : "-F";
  const r = spawnSync("stty", [flag, path, String(baud), "raw", "-echo", "-crtscts", "clocal"]);
  if (r.status !== 0) throw new Error("stty failed: " + r.stderr);
  const rd = Bun.spawn(["cat", path], { stdout: "pipe", stderr: "ignore", stdin: "ignore" });
  let cb: (d: Uint8Array) => void = () => {};
  (async () => { for await (const chunk of rd.stdout) cb(new Uint8Array(chunk)); })();
  return {
    write: (d) => { writeSync(fd, d); },
    onData: (f) => (cb = f),
    close: () => { rd.kill(); closeSync(fd); },
    tag: 1,
  };
}

const te = new TextEncoder(), td = new TextDecoder();
const u16 = (v: number) => [v & 255, (v >> 8) & 255];

export class Link {
  private waiters: { match: (f: Frame) => boolean; resolve: (f: Frame) => void }[] = [];
  onTty: (d: Uint8Array) => void = () => {};
  onTtyClose: () => void = () => {};
  onOther: (d: Uint8Array) => void = () => {};
  screen = { w: 0, h: 0, px: new Uint8Array(0), pal: new Uint16Array(256), updates: 0 };
  private nextId = 1;
  private dec: Decoder;

  private tag: number;

  constructor(private t: Transport) {
    this.tag = t.tag ?? 0;
    this.dec = new Decoder((f) => this.dispatch(f), (d) => this.onOther(d));
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
    if (f.type === 5) { s.w = dv.getUint16(0, true); s.h = dv.getUint16(2, true); s.px = new Uint8Array(s.w * s.h); }
    else if (f.type === 2) {
      const first = dv.getUint16(0, true), n = dv.getUint16(2, true);
      for (let i = 0; i < n; i++) s.pal[first + i] = dv.getUint16(4 + 2 * i, true);
    } else if (f.type === 3) {
      const x = dv.getUint16(0, true), y = dv.getUint16(2, true), w = dv.getUint16(4, true), rows = dv.getUint16(6, true);
      let o = 8;
      for (let r = 0; r < rows; r++) {
        const n = dv.getUint16(o, true); o += 2;
        let i = o, col = 0;
        const end = o + n;
        while (i < end) {
          const c = (p[i++] << 24) >> 24;
          if (c >= 0) { for (let k = 0; k <= c; k++) s.px[(y + r) * s.w + x + col++] = p[i++]; }
          else { const v = p[i++]; for (let k = 0; k < 1 - c; k++) s.px[(y + r) * s.w + x + col++] = v; }
        }
        o = end;
      }
      s.updates++;
    }
  }

  send(chan: number, type: number, payload?: Uint8Array) { this.t.write(encodeFrame(chan, type, payload)); }


  wait(match: (f: Frame) => boolean, ms = 3000): Promise<Frame> {
    return new Promise((resolve, reject) => {
      const w = { match, resolve: (f: Frame) => { clearTimeout(timer); resolve(f); } };
      const timer = setTimeout(() => { this.waiters = this.waiters.filter((x) => x !== w); reject(new Error("timeout")); }, ms);
      this.waiters.push(w);
    });
  }

  async hello(retries = 15): Promise<Record<string, unknown>> {
    for (let i = 0; i < retries; i++) {
      const p = this.wait((f) => f.chan === CH.ctl && f.type === 2, 1000);
      this.send(CH.ctl, 1, te.encode("palanu-cli"));
      try { return JSON.parse(td.decode((await p).payload)); } catch { /* retry */ }
    }
    throw new Error("no answer from the device");
  }

  /** Waits for a ctl reply of one of these types. An ERROR frame rejects with a LinkError. */
  ctlReply(types: number[], ms = 5000): Promise<Frame> {
    return this.wait((f) => f.chan === CH.ctl && (types.includes(f.type) || f.type === 0x7f), ms).then((f) => {
      if (f.type === 0x7f) throw new LinkError((f.payload[0] << 24) >> 24, td.decode(f.payload.subarray(1)));
      return f;
    });
  }

  /** Starts a pairing: sends PAIR_REQUEST and returns the session once the device has answered with its key. The
   * code is entered next with `prove`; a wrong code can be tried again on the same session (the device counts tries). */
  async pairStart(name: string): Promise<PairSession> {
    const c = x25519Keypair();
    const cn = randomBytes(16);
    const nb = te.encode(name).subarray(0, 24);
    const reply = this.ctlReply([PAIR.challenge]);
    this.send(CH.ctl, PAIR.request, Uint8Array.from([nb.length, ...nb, ...c.pub, ...cn]));
    const f = await reply;
    const dpub = f.payload.subarray(0, 32), dn = f.payload.subarray(32, 48);
    const shared = x25519(c.priv, dpub);
    const transcript = Buffer.concat([c.pub, dpub, cn, dn]);
    return new PairSession(this, shared, transcript);
  }

  /** Pairs with the device: `code` is the 6 digits the device shows (`link pair`). Returns the id and token to keep. */
  async pair(code: string, name: string): Promise<{ clientId: string; token: string }> {
    return (await this.pairStart(name)).prove(code);
  }

  /** Authenticates this connection with a stored client id and token (AUTH_HELLO, AUTH_RESPONSE). */
  async auth(clientId: string, token: string): Promise<void> {
    const id = fromHex(clientId), tok = fromHex(token);
    const challenge = this.ctlReply([AUTH.challenge]);
    this.send(CH.ctl, AUTH.hello, id);
    const ch = await challenge;
    const ok = this.ctlReply([AUTH.ok]);
    this.send(CH.ctl, AUTH.response, authResponse(tok, ch.payload.subarray(0, 16), id, this.tag));
    await ok;
  }

  async ping(): Promise<number> {
    const t0 = performance.now();
    const p = this.wait((f) => f.chan === CH.ctl && f.type === 4);
    this.send(CH.ctl, 3, new Uint8Array([1, 2, 3, 4]));
    await p;
    return performance.now() - t0;
  }

  // ---- shell ----
  openShell(cols: number, rows: number) { this.send(CH.tty, 1, Uint8Array.from([...u16(cols), ...u16(rows)])); }
  resize(cols: number, rows: number) { this.send(CH.tty, 3, Uint8Array.from([...u16(cols), ...u16(rows)])); }
  ttyWrite(d: Uint8Array) { for (let i = 0; i < d.length; i += 512) this.send(CH.tty, 2, d.subarray(i, i + 512)); }
  closeShell() { this.send(CH.tty, 4); }

  // ---- files ----
  private idFrame(id: number, rest: Uint8Array | string = new Uint8Array(0)) {
    const r = typeof rest === "string" ? te.encode(rest) : rest;
    return Uint8Array.from([id, ...r]);
  }
  private fsErr(f: Frame): Error {
    const code = (f.payload[1] << 24) >> 24;
    return new Error(`device error ${code}`);
  }

  async list(path: string): Promise<{ name: string; dir: boolean; size: number }[]> {
    const id = this.nextId++ & 255, out: { name: string; dir: boolean; size: number }[] = [];
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("timeout")), 5000);
      const w = { match: (f: Frame) => f.chan === CH.fs && f.payload[0] === id, resolve: (f: Frame) => {
        if (f.type === 2) { out.push({ dir: f.payload[1] === 2, size: new DataView(f.payload.buffer, f.payload.byteOffset).getUint32(2, true), name: td.decode(f.payload.subarray(6)) }); this.waiters.push(w); }
        else { clearTimeout(timer); f.type === 3 ? resolve(out) : reject(this.fsErr(f)); }
      } };
      this.waiters.push(w);
      this.send(CH.fs, 1, this.idFrame(id, path));
    });
  }

  async read(path: string): Promise<Uint8Array> {
    const id = this.nextId++ & 255, chunks: Uint8Array[] = [];
    return new Promise((resolve, reject) => {
      let timer = setTimeout(() => reject(new Error("timeout")), 5000);
      const w = { match: (f: Frame) => f.chan === CH.fs && f.payload[0] === id, resolve: (f: Frame) => {
        clearTimeout(timer);
        if (f.type === 5) { chunks.push(f.payload.slice(1)); timer = setTimeout(() => reject(new Error("timeout")), 5000); this.waiters.push(w); }
        else if (f.type === 3) resolve(Buffer.concat(chunks));
        else reject(this.fsErr(f));
      } };
      this.waiters.push(w);
      this.send(CH.fs, 4, this.idFrame(id, path));
    });
  }

  async write(path: string, data: Uint8Array): Promise<void> {
    const id = this.nextId++ & 255;
    const ack = async () => {
      const f = await this.wait((x) => x.chan === CH.fs && x.payload[0] === id, 5000);
      if (f.type !== 7) throw this.fsErr(f);
    };
    let a = ack(); this.send(CH.fs, 6, this.idFrame(id, path)); await a;
    for (let i = 0; i < data.length; i += 512) {
      a = ack(); this.send(CH.fs, 5, this.idFrame(id, data.subarray(i, i + 512))); await a;
    }
    a = ack(); this.send(CH.fs, 3, Uint8Array.of(id)); await a;
  }

  /** One fs request answered by OK or ERR: REMOVE 8, MKDIR 9, RENAME 10, INSTALL 11 (docs/PLP.md). */
  private async fsOp(type: number, payload: Uint8Array, ms = 8000): Promise<void> {
    const id = this.nextId++ & 255;
    const p = this.wait((x) => x.chan === CH.fs && x.payload[0] === id, ms);
    this.send(CH.fs, type, this.idFrame(id, payload));
    const f = await p;
    if (f.type !== 7) throw this.fsErr(f);
  }
  /** Removes a file, or an empty folder. */
  remove(path: string) { return this.fsOp(8, new TextEncoder().encode(path)); }
  mkdir(path: string) { return this.fsOp(9, new TextEncoder().encode(path)); }
  rename(from: string, to: string) {
    const te = new TextEncoder();
    return this.fsOp(10, Uint8Array.from([...te.encode(from), 0, ...te.encode(to)]));
  }
  /** Installs a .papp already on the device, as `app install` does. */
  install(path: string) { return this.fsOp(11, new TextEncoder().encode(path), 20000); }

  /** Send a firmware image; the device verifies it, switches to it, and reboots. */
  async ota(image: Uint8Array, progress: (done: number, total: number) => void = () => {}): Promise<void> {
    const step = async (type: number, payload: Uint8Array, ms = 8000) => {
      const p = this.wait((f) => f.chan === CH.ota, ms);
      this.send(CH.ota, type, payload);
      const f = await p;
      if (f.type !== 4) throw new Error(`device refused the update (code ${(f.payload[0] << 24) >> 24})`);
    };
    const n = image.length;
    await step(1, Uint8Array.from([n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255]), 20000); // erasing the slot takes a while
    for (let i = 0; i < n; i += 512) {
      await step(2, image.subarray(i, i + 512));
      if ((i / 512) % 16 === 0) progress(i, n);
    }
    progress(n, n);
    await step(3, new Uint8Array(0), 30000); // verification
  }

  // ---- screen / input ----
  subscribeScreen(on = true) { this.send(CH.screen, 1, Uint8Array.of(on ? 1 : 0)); }
  key(sym: number, cp = 0, mods = 0) { this.send(CH.input, 1, Uint8Array.from([sym, mods, cp & 255, (cp >> 8) & 255, (cp >> 16) & 255, cp >>> 24])); }

  /** The mirrored screen as a PNG. */
  screenPng(): Uint8Array {
    const { w, h, px, pal } = this.screen;
    const raw = new Uint8Array((w * 3 + 1) * h);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const c = pal[px[y * w + x]], o = y * (w * 3 + 1) + 1 + x * 3;
      raw[o] = ((c >> 11) & 31) * 255 / 31; raw[o + 1] = ((c >> 5) & 63) * 255 / 63; raw[o + 2] = (c & 31) * 255 / 31;
    }
    const chunk = (type: string, data: Uint8Array) => {
      const b = Buffer.alloc(12 + data.length);
      b.writeUInt32BE(data.length, 0); b.write(type, 4, "latin1"); Buffer.from(data).copy(b, 8);
      b.writeUInt32BE(crc32(b.subarray(4, 8 + data.length)), 8 + data.length);
      return b;
    };
    const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", new Uint8Array(0))]);
  }

  close() { this.t.close(); }
}

/** Makes an untrusted connection trusted with the token stored for this device (docs/PLP.md, "Auth"), then asks for the
 * full INFO. `hint` is the command the user runs to pair (it appears in the error when there is no token). */
export async function authenticate(link: Link, info: Record<string, unknown>, hint: string): Promise<Record<string, unknown>> {
  const p = loadPairing(String(info.id ?? ""));
  if (!p) throw new Error(`not paired: run \`${hint}\``);
  await link.auth(p.clientId, p.token);
  return link.hello();
}

/** One pairing exchange after PAIR_CHALLENGE. */
export class PairSession {
  constructor(private link: Link, private shared: Uint8Array, private transcript: Uint8Array) {}

  /** Proves the code. On success returns the client id and token (hex); a wrong code throws a LinkError with code
   * LINK_ERR.auth and the device says how many tries are left. */
  async prove(code: string): Promise<{ clientId: string; token: string }> {
    if (!/^\d{6}$/.test(code)) throw new Error("the pairing code is 6 digits");
    const { proof, kEnc } = pairKeys(this.shared, code, this.transcript);
    const ok = this.link.ctlReply([PAIR.ok]);
    this.link.send(CH.ctl, PAIR.proof, proof);
    const f = await ok;
    const id = f.payload.subarray(0, 8), nonce = f.payload.subarray(8, 20);
    const token = openToken(kEnc, id, nonce, f.payload.subarray(20, 52), f.payload.subarray(52, 68));
    return { clientId: toHex(id), token: toHex(token) };
  }
}

function crc32(b: Uint8Array): number {
  let c = ~0;
  for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1; }
  return ~c >>> 0;
}
