#!/usr/bin/env python3
"""Encodes the wasm modules of tests/unit/test_api_svc.c as C arrays (tests/unit/wasm_fixtures_svc.h). They call the kv,
settings and notify imports of the palanu module, and exit with the result of the last call, so the app's exit status is
what the test checks. The encoding is plain wasm 1.0 binary (the same helpers as gen_api_fixtures.py).

    python3 tests/unit/tools/gen_svc_fixtures.py > tests/unit/wasm_fixtures_svc.h

Each scenario is a name, a list of (module, field, type) imports, a body, and a data segment at address 0. Buffers the
calls write to live at addresses 2048 and up, which the data does not reach.
"""
import sys

I32 = 0x7F


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


def sleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def name(s):
    b = s.encode()
    return uleb(len(b)) + b


def section(sid, payload):
    return bytes([sid]) + uleb(len(payload)) + payload


# Function types, by index: 0 (i32,i32,i32)->i32, 1 (i32)->(), 2 (i32,i32)->i32, 3 (i32)->i32, 4 ()->().
T_III_I, T_I_V, T_II_I, T_I_I, T_V_V = 0, 1, 2, 3, 4
TYPES = (
    uleb(5)
    + bytes([0x60, 3, I32, I32, I32, 1, I32])
    + bytes([0x60, 1, I32, 0])
    + bytes([0x60, 2, I32, I32, 1, I32])
    + bytes([0x60, 1, I32, 1, I32])
    + bytes([0x60, 0, 0])
)

BUF = 2048  # where the calls write their results
BUF2 = 2304


def i32c(v):
    return bytes([0x41]) + sleb(v)


def call(idx):
    return bytes([0x10]) + uleb(idx)


DROP = bytes([0x1A])
LOAD8U = bytes([0x2D, 0x00, 0x00])  # i32.load8_u, align 0, offset 0


def module(imports, body, data):
    imp = uleb(len(imports)) + b"".join(name(m) + name(f) + bytes([0x00]) + uleb(t) for m, f, t in imports)
    start = len(imports)
    funcs = uleb(1) + uleb(T_V_V)
    memory = uleb(1) + bytes([0x00, 0x01])
    exports = uleb(2) + name("_start") + bytes([0x00]) + uleb(start) + name("memory") + bytes([0x02, 0x00])
    code_body = uleb(0) + body + bytes([0x0B])
    code = uleb(1) + uleb(len(code_body)) + code_body
    seg = uleb(0) + i32c(0) + bytes([0x0B]) + uleb(len(data)) + data
    return (
        b"\x00asm\x01\x00\x00\x00"
        + section(1, TYPES)
        + section(2, imp)
        + section(3, funcs)
        + section(5, memory)
        + section(7, exports)
        + section(10, code)
        + section(11, uleb(1) + seg)
    )


class Data:
    """Strings for the data segment: each add() returns its address."""

    def __init__(self):
        self.buf = bytearray()

    def add(self, b):
        if isinstance(b, str):
            b = b.encode() + b"\x00"
        addr = len(self.buf)
        self.buf += b
        assert addr + len(b) <= BUF, "data overlaps the result buffers"
        return addr

    def bytes(self):
        return bytes(self.buf)


# Imports used by the scenarios: index in the import list is the function index.
KV_GET = ("palanu", "kv_get", T_III_I)
KV_SET = ("palanu", "kv_set", T_III_I)
EXIT = ("palanu", "exit", T_I_V)
SET_GET = ("palanu", "setting_get", T_III_I)
SET_SET = ("palanu", "setting_set", T_II_I)
NOTIFY_POST = ("palanu", "notify_post", T_III_I)
TOAST = ("palanu", "toast", T_II_I)


def scenario_kv_set():
    d = Data()
    k, v = d.add("color"), d.add("blue")
    body = i32c(k) + i32c(v) + i32c(4) + call(0) + call(1)
    return module([KV_SET, EXIT], body, d.bytes())


def scenario_kv_get():
    d = Data()
    k = d.add("color")
    body = i32c(k) + i32c(BUF) + i32c(16) + call(0) + call(1)
    return module([KV_GET, EXIT], body, d.bytes())


def scenario_kv_first_byte():
    d = Data()
    k = d.add("color")
    body = i32c(k) + i32c(BUF) + i32c(16) + call(0) + DROP + i32c(BUF) + LOAD8U + call(1)
    return module([KV_GET, EXIT], body, d.bytes())


def scenario_kv_set_bad_key():
    d = Data()
    k, v = d.add("Bad Key"), d.add("x")
    body = i32c(k) + i32c(v) + i32c(1) + call(0) + call(1)
    return module([KV_SET, EXIT], body, d.bytes())


def scenario_kv_fill():
    """65 sets of new keys: the first 64 succeed, the 65th is refused with NOSPC (-17)."""
    d = Data()
    keys = [d.add("k%02d" % i) for i in range(65)]
    v = d.add("v")
    body = b""
    for i, ka in enumerate(keys):
        body += i32c(ka) + i32c(v) + i32c(1) + call(0)
        if i < 64:
            body += DROP
    body += call(1)
    return module([KV_SET, EXIT], body, d.bytes())


def scenario_setting_get_dark():
    d = Data()
    k = d.add("ui.dark")
    body = i32c(k) + i32c(BUF) + i32c(8) + call(0) + call(1)
    return module([SET_GET, EXIT], body, d.bytes())


def scenario_setting_first_char():
    d = Data()
    k = d.add("ui.dark")
    body = i32c(k) + i32c(BUF) + i32c(8) + call(0) + DROP + i32c(BUF) + LOAD8U + call(1)
    return module([SET_GET, EXIT], body, d.bytes())


def scenario_setting_set_read_only():
    d = Data()
    k, v = d.add("radio.ble"), d.add("on")
    body = i32c(k) + i32c(v) + call(0) + call(1)
    return module([SET_SET, EXIT], body, d.bytes())


def scenario_setting_set_dark():
    d = Data()
    k, v = d.add("ui.dark"), d.add("1")
    body = i32c(k) + i32c(v) + call(0) + call(1)
    return module([SET_SET, EXIT], body, d.bytes())


def scenario_notify_post():
    d = Data()
    t, x = d.add("backup"), d.add("done")
    body = i32c(t) + i32c(x) + i32c(0) + call(0) + call(1)
    return module([NOTIFY_POST, EXIT], body, d.bytes())


def scenario_notify_rate():
    """Four posts, then a fifth: the fifth is refused with BUSY (-4)."""
    d = Data()
    t, x = d.add("tick"), d.add("x")
    body = b""
    for _ in range(4):
        body += i32c(t) + i32c(x) + i32c(0) + call(0) + DROP
    body += i32c(t) + i32c(x) + i32c(0) + call(0) + call(1)
    return module([NOTIFY_POST, EXIT], body, d.bytes())


def scenario_toast_short():
    d = Data()
    x = d.add("hi")
    body = i32c(x) + i32c(50) + call(0) + call(1)
    return module([TOAST, EXIT], body, d.bytes())


SCENARIOS = [
    ("SVC_KV_SET", scenario_kv_set),
    ("SVC_KV_GET", scenario_kv_get),
    ("SVC_KV_FIRST", scenario_kv_first_byte),
    ("SVC_KV_BAD_KEY", scenario_kv_set_bad_key),
    ("SVC_KV_FILL", scenario_kv_fill),
    ("SVC_SETTING_GET", scenario_setting_get_dark),
    ("SVC_SETTING_FIRST", scenario_setting_first_char),
    ("SVC_SETTING_SET_RO", scenario_setting_set_read_only),
    ("SVC_SETTING_SET_DARK", scenario_setting_set_dark),
    ("SVC_NOTIFY_POST", scenario_notify_post),
    ("SVC_NOTIFY_RATE", scenario_notify_rate),
    ("SVC_TOAST_SHORT", scenario_toast_short),
]


def c_array(name_, data):
    lines = [f"static const uint8_t {name_}[{len(data)}] = {{"]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out = [
        "/* Hand-encoded wasm modules for tests/unit/test_api_svc.c, GENERATED by tests/unit/tools/gen_svc_fixtures.py.",
        " * Do not edit: change the generator and run it again. */",
        "#ifndef WASM_FIXTURES_SVC_H",
        "#define WASM_FIXTURES_SVC_H",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
    ]
    names = []
    for cname, fn in SCENARIOS:
        out += [c_array(cname, fn()), ""]
        names.append(cname)
    out += ["typedef struct {", "    const char *name;", "    const uint8_t *mod;", "    size_t len;", "} svc_fixture_t;", ""]
    out.append("static const svc_fixture_t SVC_FIXTURES[] = {")
    for cname in names:
        out.append(f'    {{"{cname}", {cname}, sizeof({cname})}},')
    out += ["};", "", "#endif", ""]
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
