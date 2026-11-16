// Palanu Link pairing and auth, the client side (docs/PLP.md, "Pairing" and "Auth"). The primitives are Node's: X25519,
// HMAC-SHA256 and ChaCha20-Poly1305 are the ones the device derives the same way (system/src/link_auth.c).
import { aeadOpen } from "./chacha.ts";
import { createHmac, createPrivateKey, createPublicKey, diffieHellman, generateKeyPairSync, randomBytes } from "node:crypto";

export const PAIR = { request: 5, challenge: 6, proof: 7, ok: 8 } as const;
export const AUTH = { hello: 9, challenge: 10, response: 11, ok: 12 } as const;
/** Device error codes (NEMA_ERR_*, and PAL_LINK_E_PAIR_CLOSED) that a client reacts to. */
export const LINK_ERR = { auth: -23, busy: -4, full: -15, state: -16, pairClosed: -27 } as const;

const SPKI_PREFIX = Buffer.from("302a300506032b656e032100", "hex"); // X25519 public key, DER
const PKCS8_PREFIX = Buffer.from("302e020100300506032b656e04220420", "hex"); // X25519 private key, DER

export function x25519Keypair(): { priv: Uint8Array; pub: Uint8Array } {
  const { privateKey, publicKey } = generateKeyPairSync("x25519");
  const pub = publicKey.export({ format: "der", type: "spki" }).subarray(-32);
  const priv = privateKey.export({ format: "der", type: "pkcs8" }).subarray(-32);
  return { priv: new Uint8Array(priv), pub: new Uint8Array(pub) };
}

export function x25519(priv: Uint8Array, pub: Uint8Array): Uint8Array {
  const k = createPrivateKey({ key: Buffer.concat([PKCS8_PREFIX, Buffer.from(priv)]), format: "der", type: "pkcs8" });
  const p = createPublicKey({ key: Buffer.concat([SPKI_PREFIX, Buffer.from(pub)]), format: "der", type: "spki" });
  return new Uint8Array(diffieHellman({ privateKey: k, publicKey: p }));
}

export function hmac(key: Uint8Array, ...parts: Uint8Array[]): Uint8Array {
  const h = createHmac("sha256", key);
  for (const p of parts) h.update(p);
  return new Uint8Array(h.digest());
}

const te = new TextEncoder();
const label = (s: string) => te.encode(s);
const cat = (...xs: Uint8Array[]) => Buffer.concat(xs.map((x) => Buffer.from(x)));

/** The keys of one pairing exchange. transcript = cpub || dpub || cn || dn, as the device builds it. */
export function pairKeys(shared: Uint8Array, code: string, transcript: Uint8Array) {
  const prk = hmac(label("palanu-pair-v1"), shared, label(code));
  const kMac = hmac(prk, label("mac"), transcript);
  const kEnc = hmac(prk, label("enc"), transcript);
  return { proof: hmac(kMac, label("pair-proof"), transcript), kEnc };
}

export function openToken(kEnc: Uint8Array, clientId: Uint8Array, nonce: Uint8Array, ct: Uint8Array, tag: Uint8Array): Uint8Array {
  const token = aeadOpen(kEnc, nonce, cat(label("palanu-token"), clientId), ct, tag);
  if (!token) throw new Error("the device's token does not verify: the pairing was not with this device's key");
  return token;
}

/** AUTH_RESPONSE: HMAC-SHA256(token, nonce || client id || transport tag). */
export function authResponse(token: Uint8Array, nonce: Uint8Array, clientId: Uint8Array, transportTag: number): Uint8Array {
  return hmac(token, nonce, clientId, Uint8Array.of(transportTag));
}

export const toHex = (b: Uint8Array) => Buffer.from(b).toString("hex");
export const fromHex = (s: string) => new Uint8Array(Buffer.from(s, "hex"));
export { randomBytes };
