#!/usr/bin/env python3
"""Writes tests/unit/wasm_crypto_fixtures.h: hand-encoded WebAssembly modules that call the crypto imports of the palanu
module. Run from the repository root:

    python3 tests/unit/tools/gen_wasm_crypto_fixtures.py

The modules are small enough to write out by hand, so the test needs no toolchain: this script is the record of what
each module does. Each module exports `_start` and `memory`; its exit status is what the test checks.
"""
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "tests", "unit", "wasm_crypto_fixtures.h")

I32 = 0x7F
TYPES = {  # name -> (params, results)
    "exit": ([I32], []),
    "one": ([I32], [I32]),  # hash_open, aes_ctr_open-like
    "three": ([I32, I32, I32], [I32]),  # hash_update, hash_final
    "nine": ([I32] * 9, [I32]),  # aead_seal, aead_decrypt
    "void": ([], []),  # _start
    "aead_open": ([I32, I32, I32], [I32]),
    "two": ([I32, I32], [I32]),  # key_list(out, cap)
}


def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def sleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        done = (n == 0 and not (b & 0x40)) or (n == -1 and (b & 0x40))
        out.append(b if done else b | 0x80)
        if done:
            return bytes(out)


def vec(items):
    return uleb(len(items)) + b"".join(items)


def name(s):
    b = s.encode()
    return uleb(len(b)) + b


def section(sid, body):
    return bytes([sid]) + uleb(len(body)) + body


def functype(params, results):
    return b"\x60" + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])


class Module:
    """A module with imports from the palanu module, one function _start, one memory page, and data segments."""

    def __init__(self, imports, data):
        # imports: list of (import name, type key); the index of an import is its position in this list
        self.imports = imports
        self.data = data  # list of (offset, bytes)

    def build(self, body_code, extra_locals=0):
        types = []
        typeidx = {}
        for key in ["exit", "one", "three", "nine", "void", "aead_open", "two"]:
            typeidx[key] = len(types)
            types.append(functype(*TYPES[key]))
        imp = []
        for nm, key in self.imports:
            imp.append(name("palanu") + name(nm) + b"\x00" + uleb(typeidx[key]))
        start_type = typeidx["void"]
        func_section = uleb(start_type)
        mem = b"\x00\x01"  # limits: no maximum, minimum 1 page (64 KB); the section adds the count
        exports = vec([name("_start") + b"\x00" + uleb(len(self.imports)), name("memory") + b"\x02\x00"])
        locals_decl = vec([uleb(1) + bytes([I32])] if extra_locals else [])
        code = locals_decl + body_code + b"\x0b"
        codes = vec([uleb(len(code)) + code])
        datas = vec([b"\x00\x41" + sleb(off) + b"\x0b" + uleb(len(bts)) + bts for off, bts in self.data])
        out = b"\x00asm" + struct.pack("<I", 1)
        out += section(1, vec(types))
        out += section(2, vec(imp))
        out += section(3, vec([func_section]))
        out += section(5, vec([mem]))
        out += section(7, exports)
        out += section(10, codes)
        out += section(11, datas)
        return out


def i32(v):
    return b"\x41" + sleb(v)


def call(idx):
    return b"\x10" + uleb(idx)


LOCAL_GET = b"\x20\x00"
LOCAL_SET = b"\x21\x00"
DROP = b"\x1a"
LOAD8U = b"\x2d\x00\x00"  # align 0, offset 0


def emit_c(var, data):
    return f"static const uint8_t {var}[{len(data)}] = {{\n" + "\n".join(
        "    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + "," for i in range(0, len(data), 12)
    ) + "\n};\n"


def fixtures():
    out = {}

    # HASH_ABC: SHA-256 of "abc" through the hash handle; exits with the first digest byte (0xba).
    m = Module([("exit", "exit"), ("hash_open", "one"), ("hash_update", "three"), ("hash_final", "three")],
               [(0, b"abc")])
    body = i32(2) + call(1) + LOCAL_SET + LOCAL_GET + i32(0) + i32(3) + call(2) + DROP \
        + LOCAL_GET + i32(64) + i32(32) + call(3) + DROP + i32(64) + LOAD8U + call(0)
    out["WASM_HASH_ABC"] = m.build(body, extra_locals=1)

    # HASH_LEAK: three handles opened and never finished; the app exits 0. The sweep at exit must free them.
    m = Module([("exit", "exit"), ("hash_open", "one")], [])
    body = (i32(2) + call(1) + DROP) * 3 + i32(0) + call(0)
    out["WASM_HASH_LEAK"] = m.build(body)

    # AEAD_RFC8439: ChaCha20-Poly1305 seal of RFC 8439 section 2.8.2 (key, nonce, aad, plaintext laid out at fixed
    # offsets); exits with the first tag byte (0x1a).
    key = bytes(range(0x80, 0xA0))
    nonce = bytes.fromhex("070000004041424344454647")
    aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
    pt = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
          b"sunscreen would be it.")
    assert len(pt) == 114, len(pt)
    m = Module([("exit", "exit"), ("aead_open", "aead_open"), ("aead_seal", "nine")],
               [(0, key), (32, nonce), (48, aad), (64, pt)])
    body = i32(3) + i32(0) + i32(32) + call(1) + LOCAL_SET \
        + LOCAL_GET + i32(32) + i32(12) + i32(48) + i32(12) + i32(64) + i32(114) + i32(200) + i32(16) + call(2) + DROP \
        + i32(200) + LOAD8U + call(0)
    out["WASM_AEAD_RFC8439"] = m.build(body, extra_locals=1)
    # KEY_LIST: lists the keystore through key_list(out, cap) and exits with the result. Without the crypto.keys
    # permission the glue refuses the call (NEMA_ERR_PERM, -9, which is the exit status); with it, the list is empty here.
    m = Module([("exit", "exit"), ("key_list", "two")], [])
    body = i32(0) + i32(16) + call(1) + call(0)
    out["WASM_KEY_LIST"] = m.build(body)
    return out


def main():
    lines = [
        "/* GENERATED by tests/unit/tools/gen_wasm_crypto_fixtures.py: do not edit, run the script. Hand-encoded wasm modules",
        " * for tests/unit/test_wasm_crypto.c. */",
        "#ifndef WASM_CRYPTO_FIXTURES_H",
        "#define WASM_CRYPTO_FIXTURES_H",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
    ]
    for var, data in fixtures().items():
        lines.append(emit_c(var, data).rstrip("\n"))
        lines.append("")
    lines.append("#endif")
    lines.append("")
    with open(OUT, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
