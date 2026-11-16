// ChaCha20-Poly1305 (RFC 8439) for the pairing token. Bun's node:crypto has no such cipher, so this is the small reference
// version: it opens what the device seals (system/src/cryptoimpl and Monocypher), and the tests check it against the RFC's
// vectors. Only opening is needed by the client.

const rotl = (v: number, c: number) => (v << c) | (v >>> (32 - c));

function quarter(x: Uint32Array, a: number, b: number, c: number, d: number) {
  x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 16);
  x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 12);
  x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 8);
  x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 7);
}

/** One 64-byte ChaCha20 keystream block (RFC 8439 section 2.3). */
export function chacha20Block(key: Uint8Array, counter: number, nonce: Uint8Array): Uint8Array {
  const dv = new DataView(key.buffer, key.byteOffset, key.byteLength);
  const nv = new DataView(nonce.buffer, nonce.byteOffset, nonce.byteLength);
  const s = new Uint32Array(16);
  s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
  for (let i = 0; i < 8; i++) s[4 + i] = dv.getUint32(4 * i, true);
  s[12] = counter;
  for (let i = 0; i < 3; i++) s[13 + i] = nv.getUint32(4 * i, true);
  const x = Uint32Array.from(s);
  for (let r = 0; r < 10; r++) {
    quarter(x, 0, 4, 8, 12); quarter(x, 1, 5, 9, 13); quarter(x, 2, 6, 10, 14); quarter(x, 3, 7, 11, 15);
    quarter(x, 0, 5, 10, 15); quarter(x, 1, 6, 11, 12); quarter(x, 2, 7, 8, 13); quarter(x, 3, 4, 9, 14);
  }
  const out = new Uint8Array(64), ov = new DataView(out.buffer);
  for (let i = 0; i < 16; i++) ov.setUint32(4 * i, (x[i] + s[i]) >>> 0, true);
  return out;
}

/** XORs the keystream from `counter` over data (encryption and decryption are the same operation). */
export function chacha20Xor(key: Uint8Array, counter: number, nonce: Uint8Array, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(data.length);
  for (let off = 0, b = 0; off < data.length; off += 64, b++) {
    const ks = chacha20Block(key, counter + b, nonce);
    for (let i = 0; i < 64 && off + i < data.length; i++) out[off + i] = data[off + i] ^ ks[i];
  }
  return out;
}

const P = (1n << 130n) - 5n;
const MASK128 = (1n << 128n) - 1n;

function leToBig(b: Uint8Array): bigint {
  let v = 0n;
  for (let i = b.length - 1; i >= 0; i--) v = (v << 8n) | BigInt(b[i]);
  return v;
}

/** Poly1305 one-time MAC (RFC 8439 section 2.5). */
export function poly1305(key: Uint8Array, msg: Uint8Array): Uint8Array {
  const r = leToBig(key.subarray(0, 16)) & 0x0ffffffc0ffffffc0ffffffc0fffffffn;
  const s = leToBig(key.subarray(16, 32));
  let h = 0n;
  for (let i = 0; i < msg.length; i += 16) {
    const block = msg.subarray(i, Math.min(i + 16, msg.length));
    const n = leToBig(block) + (1n << BigInt(8 * block.length));
    h = ((h + n) * r) % P;
  }
  h = (h + s) & MASK128;
  const out = new Uint8Array(16);
  for (let i = 0; i < 16; i++) { out[i] = Number(h & 0xffn); h >>= 8n; }
  return out;
}

const pad16 = (n: number) => (16 - (n % 16)) % 16;

/** The AEAD tag over aad and ciphertext (RFC 8439 section 2.8). */
function aeadTag(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, ct: Uint8Array): Uint8Array {
  const otk = chacha20Block(key, 0, nonce).subarray(0, 32);
  const macData = new Uint8Array(aad.length + pad16(aad.length) + ct.length + pad16(ct.length) + 16);
  macData.set(aad, 0);
  let o = aad.length + pad16(aad.length);
  macData.set(ct, o);
  o += ct.length + pad16(ct.length);
  const lens = new DataView(macData.buffer, o, 16);
  lens.setUint32(0, aad.length, true);
  lens.setUint32(8, ct.length, true);
  return poly1305(otk, macData);
}

/** Opens a sealed message: returns the plaintext, or null when the tag does not verify. */
export function aeadOpen(key: Uint8Array, nonce: Uint8Array, aad: Uint8Array, ct: Uint8Array, tag: Uint8Array): Uint8Array | null {
  const want = aeadTag(key, nonce, aad, ct);
  let diff = 0;
  for (let i = 0; i < 16; i++) diff |= want[i] ^ tag[i];
  if (diff !== 0) return null;
  return chacha20Xor(key, 1, nonce, ct);
}
