// Minimal PNG and PPM codecs for the golden-image tool: 8-bit RGB, no interlace, no palette.
// Encoding writes filter 0 on every row; decoding accepts all five filter types so references made by other
// tools still load. Only node:zlib is used.
import { deflateSync, inflateSync } from "node:zlib";

export interface Image {
  w: number;
  h: number;
  rgb: Buffer; // w * h * 3 bytes, row-major
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of bytes) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Uint8Array): Buffer {
  const head = Buffer.alloc(8);
  head.writeUInt32BE(data.length, 0);
  head.write(type, 4, "latin1");
  const body = Buffer.concat([head.subarray(4), Buffer.from(data)]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([head.subarray(0, 4), body, crc]);
}

export function encodePNG(img: Image): Buffer {
  const stride = img.w * 3;
  const raw = Buffer.alloc((stride + 1) * img.h); // filter byte 0 at the start of each row
  for (let y = 0; y < img.h; y++) img.rgb.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(img.w, 0);
  ihdr.writeUInt32BE(img.h, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // colour type: truecolour RGB
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw, { level: 9 })),
    chunk("IEND", new Uint8Array(0)),
  ]);
}

export function decodePNG(buf: Buffer): Image {
  let pos = 8;
  let w = 0;
  let h = 0;
  const idat: Buffer[] = [];
  while (pos + 8 <= buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString("latin1", pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    pos += 12 + len;
    if (type === "IHDR") {
      w = data.readUInt32BE(0);
      h = data.readUInt32BE(4);
      if (data[8] !== 8 || data[9] !== 2 || data[12] !== 0) throw new Error("PNG: only 8-bit non-interlaced RGB is supported");
    } else if (type === "IDAT") idat.push(data);
    else if (type === "IEND") break;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const bpp = 3;
  const stride = w * bpp;
  const out = Buffer.alloc(stride * h);
  for (let y = 0; y < h; y++) {
    const filter = raw[y * (stride + 1)];
    const src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const row = out.subarray(y * stride, (y + 1) * stride);
    const prev = y > 0 ? out.subarray((y - 1) * stride, y * stride) : null;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? row[x - bpp] : 0;
      const b = prev ? prev[x] : 0;
      const c = prev && x >= bpp ? prev[x - bpp] : 0;
      let v = src[x];
      switch (filter) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: {
          const p = a + b - c;
          const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
          v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
          break;
        }
        default: throw new Error(`PNG: unknown filter ${filter}`);
      }
      row[x] = v & 0xff;
    }
  }
  return { w, h, rgb: out };
}

// PPM (P6, 8-bit) as written by palanu-sim --shot.
export function parsePPM(buf: Buffer): Image {
  const tokens: string[] = [];
  let pos = 0;
  const isSpace = (c: number) => c === 0x20 || c === 0x0a || c === 0x0d || c === 0x09;
  while (tokens.length < 4) {
    while (pos < buf.length && isSpace(buf[pos])) pos++;
    let t = "";
    while (pos < buf.length && !isSpace(buf[pos])) t += String.fromCharCode(buf[pos++]);
    if (t === "") throw new Error("PPM: truncated header");
    tokens.push(t);
  }
  pos++; // exactly one whitespace byte separates the header from the pixels
  if (tokens[0] !== "P6" || tokens[3] !== "255") throw new Error(`PPM: expected binary P6 with maxval 255, got ${tokens[0]} ${tokens[3]}`);
  const w = Number(tokens[1]);
  const h = Number(tokens[2]);
  const rgb = Buffer.from(buf.subarray(pos, pos + w * h * 3));
  if (rgb.length !== w * h * 3) throw new Error("PPM: pixel data is short");
  return { w, h, rgb };
}
