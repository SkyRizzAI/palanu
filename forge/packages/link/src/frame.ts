// Palanu Link frames (docs/PLP.md, "Bingkai"): no Node or Bun API here, so the command-line client (index.ts) and Web
// Forge in the browser (forge/web) share it.
export const CH = { ctl: 0, tty: 1, screen: 2, input: 3, fs: 4, ota: 5 } as const;
export const MAGIC = 0xab;
export const MAX_PAYLOAD = 1024;

export function crc16(buf: Uint8Array, crc = 0xffff): number {
  for (const b of buf) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

export function encodeFrame(chan: number, type: number, payload: Uint8Array = new Uint8Array(0)): Uint8Array {
  if (payload.length > MAX_PAYLOAD) throw new Error("payload too large");
  const f = new Uint8Array(7 + payload.length);
  f[0] = MAGIC; f[1] = chan; f[2] = type; f[3] = payload.length & 255; f[4] = payload.length >> 8;
  f.set(payload, 5);
  const c = crc16(f.subarray(1, 5 + payload.length));
  f[5 + payload.length] = c & 255; f[6 + payload.length] = c >> 8;
  return f;
}

export type Frame = { chan: number; type: number; payload: Uint8Array };

/** Splits a byte stream into frames; bytes outside frames come out through `onOther` (the plain console). */
export class Decoder {
  private buf: number[] = [];
  constructor(private onFrame: (f: Frame) => void, private onOther: (b: Uint8Array) => void = () => {}) {}
  push(data: Uint8Array) {
    const b = this.buf;
    for (const x of data) b.push(x);
    for (;;) {
      if (b.length === 0) return;
      if (b[0] !== MAGIC) {
        let i = b.indexOf(MAGIC);
        if (i < 0) i = b.length;
        this.onOther(Uint8Array.from(b.splice(0, i)));
        continue;
      }
      if (b.length < 5) return;
      const len = b[3] | (b[4] << 8);
      if (len > MAX_PAYLOAD) { b.shift(); continue; }
      if (b.length < 7 + len) return;
      const body = Uint8Array.from(b.slice(1, 5 + len));
      const want = b[5 + len] | (b[6 + len] << 8);
      if (crc16(body) === want) {
        b.splice(0, 7 + len);
        this.onFrame({ chan: body[0], type: body[1], payload: body.subarray(4) });
      } else b.shift(); // resynchronise on the next magic byte
    }
  }
}

