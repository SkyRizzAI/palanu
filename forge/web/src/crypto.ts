// Pairing and auth crypto in the browser (docs/PLP.md, "Pairing" and "Auth"): X25519 and HMAC-SHA256 from WebCrypto,
// ChaCha20-Poly1305 from the command-line client's small reference implementation (WebCrypto has no such cipher). The
// derivations are the same as forge/packages/link/src/auth.ts and the device's system/src/link_auth.c.
import { aeadOpen } from "../../packages/link/src/chacha.ts";

const te = new TextEncoder();
const subtle = () => {
  if (!globalThis.crypto?.subtle) throw new Error("this page needs a secure context (open it from a file or http://localhost)");
  return globalThis.crypto.subtle;
};

export const randomBytes = (n: number) => crypto.getRandomValues(new Uint8Array(n));

export function concat(...xs: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(xs.reduce((a, x) => a + x.length, 0));
  let o = 0;
  for (const x of xs) { out.set(x, o); o += x.length; }
  return out;
}

export async function x25519Keypair(): Promise<{ priv: CryptoKey; pub: Uint8Array }> {
  let kp: CryptoKeyPair;
  try {
    kp = (await subtle().generateKey({ name: "X25519" }, true, ["deriveBits"])) as CryptoKeyPair;
  } catch {
    throw new Error("this browser has no X25519 in WebCrypto (needs Chrome 133, Edge 133, Safari 17 or Firefox 130)");
  }
  return { priv: kp.privateKey, pub: new Uint8Array(await subtle().exportKey("raw", kp.publicKey)) };
}

export async function x25519(priv: CryptoKey, pub: Uint8Array): Promise<Uint8Array> {
  const p = await subtle().importKey("raw", pub as BufferSource, { name: "X25519" }, true, []);
  return new Uint8Array(await subtle().deriveBits({ name: "X25519", public: p } as EcdhKeyDeriveParams, priv, 256));
}

export async function hmac(key: Uint8Array, ...parts: Uint8Array[]): Promise<Uint8Array> {
  const k = await subtle().importKey("raw", key as BufferSource, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return new Uint8Array(await subtle().sign("HMAC", k, concat(...parts) as BufferSource));
}

/** The keys of one pairing exchange. transcript = cpub || dpub || cn || dn, as the device builds it. */
export async function pairKeys(shared: Uint8Array, code: string, transcript: Uint8Array) {
  const prk = await hmac(te.encode("palanu-pair-v1"), shared, te.encode(code));
  const kMac = await hmac(prk, te.encode("mac"), transcript);
  const kEnc = await hmac(prk, te.encode("enc"), transcript);
  return { proof: await hmac(kMac, te.encode("pair-proof"), transcript), kEnc };
}

export function openToken(kEnc: Uint8Array, clientId: Uint8Array, nonce: Uint8Array, ct: Uint8Array, tag: Uint8Array): Uint8Array {
  const token = aeadOpen(kEnc, nonce, concat(te.encode("palanu-token"), clientId), ct, tag);
  if (!token) throw new Error("the device's token does not verify");
  return token;
}

/** AUTH_RESPONSE: HMAC-SHA256(token, nonce || client id || transport tag). */
export const authResponse = (token: Uint8Array, nonce: Uint8Array, clientId: Uint8Array, tag: number) =>
  hmac(token, nonce, clientId, Uint8Array.of(tag));

export const toHex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
export const fromHex = (s: string) => Uint8Array.from(s.match(/../g) ?? [], (h) => parseInt(h, 16));
