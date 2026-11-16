// The ChaCha20-Poly1305 reference (chacha.ts): RFC 8439 vectors for Poly1305 and the block function, and a message the device
// sealed (Monocypher, the same primitive as system/src/crypto_prim.c), which must open here and fail when altered.
import { expect, test } from "bun:test";
import { aeadOpen, chacha20Block, poly1305 } from "../src/chacha.ts";

const hex = (s: string) => new Uint8Array(s.replace(/[^0-9a-f]/gi, "").match(/../g)!.map((b) => parseInt(b, 16)));
const toHex = (b: Uint8Array) => Buffer.from(b).toString("hex");

test("poly1305 (RFC 8439 2.5.2)", () => {
  const key = hex("85:d6:be:78:57:55:6d:33:7f:44:52:fe:42:d5:06:a8:01:03:80:8a:fb:0d:b2:fd:4a:bf:f6:af:41:49:f5:1b");
  const msg = new TextEncoder().encode("Cryptographic Forum Research Group");
  expect(toHex(poly1305(key, msg))).toBe("a8061dc1305136c6c22b8baf0c0127a9");
});

test("chacha20 block (RFC 8439 2.3.2)", () => {
  const key = hex("00:01:02:03:04:05:06:07:08:09:0a:0b:0c:0d:0e:0f:10:11:12:13:14:15:16:17:18:19:1a:1b:1c:1d:1e:1f");
  const nonce = hex("00:00:00:09:00:00:00:4a:00:00:00:00");
  expect(toHex(chacha20Block(key, 1, nonce)).slice(0, 32)).toBe("10f1e7e4d13b5915500fdd1fa32071c4");
});

test("aead open of a message the device sealed (Monocypher, the device's own primitive)", () => {
  // key 30..4f, nonce 90..9b, aad "palanu-token" || a0..a7, plaintext "0123456789abcdef" twice
  const key = Uint8Array.from({ length: 32 }, (_, i) => 0x30 + i);
  const nonce = Uint8Array.from({ length: 12 }, (_, i) => 0x90 + i);
  const aad = new Uint8Array([...new TextEncoder().encode("palanu-token"), 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7]);
  const ct = hex("668ed4ffe75c4a6e4c3bafccc328356d9bf7a597061e05f8a6289a4907c49513");
  const tag = hex("aed257df293cf02b2f7f824f93edabc8");
  expect(new TextDecoder().decode(aeadOpen(key, nonce, aad, ct, tag)!)).toBe("0123456789abcdef0123456789abcdef");
  const bad = ct.slice();
  bad[0] ^= 1;
  expect(aeadOpen(key, nonce, aad, bad, tag)).toBeNull();
});
