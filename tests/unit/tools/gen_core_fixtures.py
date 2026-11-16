#!/usr/bin/env python3
"""Encodes the wasm apps of tests/unit/test_api_events.c and tests/unit/test_api_proc.c as C arrays
(tests/unit/wasm_fixtures_core.h). Each app calls palanu imports from _start and writes its results to its stdout (fd 1)
as raw bytes, or exits with a status; the tests read those bytes back. Run from the repo root:

    python3 tests/unit/tools/gen_core_fixtures.py > tests/unit/wasm_fixtures_core.h

The encoding is plain wasm 1.0 binary: type, import, function, memory, export, code, and data sections. Memory map of
every app (the same in all of them, see the constants below):
  0x000..0x0FF  NUL-terminated strings (keys, messages, names)
  0x100..0x10F  wait records (8 bytes each: int32 handle, uint16 want, uint16 got)
  0x200..0x2FF  results (int32 each)
  0x300..0x3FF  values (getenv results, localtime, status)
  0x400..0x4FF  event records and a scratch byte
"""
import sys

# ---- binary encoding ----


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


VT = {"i32": 0x7F, "i64": 0x7E}

# The import table: the wasm name of each palanu function and its type, as the runtime registers them
# (api_wasm_glue.c; the strings after the names are the native signatures).
SIGS = {
    "write": (["i32"] * 3, ["i32"]),
    "read": (["i32"] * 3, ["i32"]),
    "exit": (["i32"], []),
    "argc": ([], ["i32"]),
    "wait": (["i32"] * 3, ["i32"]),
    "events": ([], ["i32"]),
    "event_next": (["i32"] * 3, ["i32"]),
    "spawn": (["i32"] * 4, ["i32"]),
    "child_wait": (["i32"] * 4, ["i32"]),
    "signal": (["i32"] * 2, ["i32"]),
    "getenv": (["i32"] * 3, ["i32"]),
    "unix_time": ([], ["i64"]),
    "localtime": (["i64", "i32", "i32"], ["i32"]),
    "log": (["i32", "i32"], ["i32"]),
    "tty_raw": (["i32"], ["i32"]),
    "tty_get": ([], ["i32"]),
    "term_size": (["i32", "i32"], ["i32"]),
    "tty_caps": ([], ["i32"]),
    "sleep_ms": (["i32"], []),
}

DATA_SIZE = 0x600


class Fn:
    """The body of _start: instructions in the order they run, with the palanu imports they call."""

    def __init__(self):
        self.code = bytearray()
        self.imports = []  # wasm names, in import order

    def _idx(self, nm):
        if nm not in SIGS:
            raise KeyError(nm)
        if nm not in self.imports:
            self.imports.append(nm)
        return self.imports.index(nm)

    def i32(self, v):
        self.code += b"\x41" + sleb(v)

    def i64(self, v):
        self.code += b"\x42" + sleb(v)

    def call(self, nm):
        self.code += b"\x10" + uleb(self._idx(nm))

    def drop(self):
        self.code += b"\x1a"

    def add(self):
        self.code += b"\x6a"

    def ld32(self):  # i32.load (address on the stack)
        self.code += b"\x28\x02\x00"

    def st32(self):  # i32.store (address, then value)
        self.code += b"\x36\x02\x00"

    def st64(self):  # i64.store (address, then value)
        self.code += b"\x37\x03\x00"

    # Helpers that pair a call with the store of its result at addr (0x200 and up).
    def store_call(self, addr, nm, *args):
        self.i32(addr)
        for a in args:
            self.i32(a)
        self.call(nm)
        self.st32()

    def write(self, fd, addr, n):
        self.i32(fd)
        self.i32(addr)
        self.i32(n)
        self.call("write")
        self.drop()


def build(body, data_items):
    """body(f) emits _start; data_items: (address, bytes) placed in the one memory."""
    f = Fn()
    body(f)
    f.code += b"\x0b"  # end of _start
    types = []

    def type_index(params, results):
        key = (tuple(params), tuple(results))
        if key not in types:
            types.append(key)
        return types.index(key)

    imports = []
    for nm in f.imports:
        ps, rs = SIGS[nm]
        imports.append((nm, type_index(ps, rs)))
    start_type = type_index([], [])
    type_sec = uleb(len(types)) + b"".join(
        bytes([0x60]) + uleb(len(ps)) + bytes(VT[p] for p in ps) + uleb(len(rs)) + bytes(VT[r] for r in rs)
        for ps, rs in types
    )
    imp_sec = uleb(len(imports)) + b"".join(name("palanu") + name(nm) + b"\x00" + uleb(t) for nm, t in imports)
    func_sec = uleb(1) + uleb(start_type)
    mem_sec = uleb(1) + b"\x00" + uleb(1)
    start_index = len(imports)
    exp_sec = uleb(2) + name("memory") + b"\x02\x00" + name("_start") + b"\x00" + uleb(start_index)
    body_bytes = uleb(0) + bytes(f.code)
    code_sec = uleb(1) + uleb(len(body_bytes)) + body_bytes
    mem = bytearray(DATA_SIZE)
    for addr, b in data_items:
        mem[addr : addr + len(b)] = b
    last = max(addr + len(b) for addr, b in data_items)
    seg = mem[:last]
    data_sec = uleb(1) + b"\x00" + b"\x41\x00\x0b" + uleb(len(seg)) + bytes(seg)
    return (
        b"\x00asm\x01\x00\x00\x00"
        + section(1, type_sec)
        + section(2, imp_sec)
        + section(3, func_sec)
        + section(5, mem_sec)
        + section(7, exp_sec)
        + section(10, code_sec)
        + section(11, data_sec)
    )


# ---- the memory map ----
S_HOME, S_PWD, S_NOPE, S_USER, S_TERM = 0x000, 0x010, 0x020, 0x030, 0x040
S_MSG, S_KID, S_SLEEPER, S_ARGS = 0x080, 0x0A0, 0x0B0, 0x0C0
REC0, REC1 = 0x100, 0x108
RES, VAL, EV, DIG = 0x200, 0x300, 0x400, 0x500


def cstr(addr, s):
    return (addr, s.encode() + b"\x00")


def want_in(addr):
    """A wait record with handle 0 and want PAL_POLL_IN (1); the handle is stored by the app when it is the queue."""
    return (addr, bytes([0, 0, 0, 0, 1, 0, 0, 0]))


STRINGS = [
    cstr(S_HOME, "HOME"),
    cstr(S_PWD, "PWD"),
    cstr(S_NOPE, "NOPE"),
    cstr(S_USER, "USER"),
    cstr(S_TERM, "TERM"),
    cstr(S_MSG, "hello from app"),
    cstr(S_KID, "api-kid"),
    cstr(S_SLEEPER, "api-sleeper"),
    (S_ARGS, b"a\x00b\x00"),
    want_in(REC0),
    want_in(REC1),
]

# ---- the apps ----


def events_into_rec0(f):
    f.i32(REC0)
    f.call("events")
    f.st32()


def app_wait_timeout(f):  # two records (stdin, queue), 100 ms: nothing is ready
    f.i32(REC1)
    f.call("events")
    f.st32()
    f.store_call(RES, "wait", REC0, 16, 100)
    f.write(1, RES, 4)
    f.write(1, REC0, 16)


def app_wait_stdin(f):  # two records, forever: stdin becomes ready
    f.i32(REC1)
    f.call("events")
    f.st32()
    f.store_call(RES, "wait", REC0, 16, -1)
    f.write(1, RES, 4)
    f.write(1, REC0, 16)


def app_wait_bad(f):  # 12 bytes is not whole records; 17 records are too many
    f.store_call(RES, "wait", REC0, 12, 0)
    f.store_call(RES + 4, "wait", REC0, 136, 0)
    f.write(1, RES, 8)


def app_events_wait(f):  # waits on the queue, then reads one event
    events_into_rec0(f)
    f.store_call(RES, "wait", REC0, 8, -1)
    f.store_call(RES + 4, "event_next", EV, 16, 2000)
    f.write(1, RES, 8)
    f.write(1, EV, 16)


def app_tty_mode(mode):
    def body(f):
        f.store_call(RES, "tty_raw", mode)
        f.store_call(RES + 4, "read", 0, EV, 16)
        f.write(1, RES, 8)
        f.write(1, EV, 16)

    return body


def app_tty_get(f):
    f.store_call(RES, "tty_get")
    f.store_call(RES + 4, "tty_raw", 4)
    f.store_call(RES + 8, "tty_get")
    f.store_call(RES + 12, "tty_raw", 1)
    f.store_call(RES + 16, "tty_get")
    f.store_call(RES + 20, "tty_raw", 0)
    f.store_call(RES + 24, "tty_caps")
    f.write(1, RES, 28)


def app_term_size(f):
    f.store_call(RES, "term_size", VAL, 4)
    f.write(1, RES, 4)
    f.write(1, VAL, 4)


def app_env(f):
    keys = [S_HOME, S_PWD, S_NOPE, S_USER, S_TERM]
    for i, k in enumerate(keys):
        f.store_call(RES + 4 * i, "getenv", k, VAL + 32 * i, 32)
    f.write(1, RES, 4 * len(keys))
    f.write(1, VAL, 32 * len(keys))


def app_time(f):
    f.i32(RES)
    f.call("unix_time")
    f.st64()
    f.i32(RES + 8)
    f.i64(1000000000)
    f.i32(VAL)
    f.i32(36)
    f.call("localtime")
    f.st32()
    f.write(1, RES, 12)
    f.write(1, VAL, 36)


def app_log(f):
    f.store_call(RES, "log", 2, S_MSG)
    f.store_call(RES + 4, "log", 9, S_MSG)
    f.write(1, RES, 8)


def app_spawn(f):
    # spawn "api-kid" with the arguments "a" and "b"; wait for it; report the pid, the wait result and its status
    f.store_call(RES, "spawn", S_KID, S_ARGS, 4, 0)
    f.i32(RES + 4)
    f.i32(RES)
    f.ld32()
    f.i32(VAL)
    f.i32(4)
    f.i32(3000)
    f.call("child_wait")
    f.st32()
    f.write(1, RES, 8)
    f.write(1, VAL, 4)


def app_spawn_signal(sig):
    """Starts a sleeping child, sends it sig, and waits for it: the results are the spawn pid, the signal's result, the
    wait's result, and the child's status."""

    def body(f):
        f.store_call(RES, "spawn", S_SLEEPER, 0, 0, 0)
        f.i32(RES + 4)
        f.i32(RES)
        f.ld32()
        f.i32(sig)
        f.call("signal")
        f.st32()
        f.i32(RES + 8)
        f.i32(RES)
        f.ld32()
        f.i32(VAL)
        f.i32(4)
        f.i32(5000)
        f.call("child_wait")
        f.st32()
        f.write(1, RES, 12)
        f.write(1, VAL, 4)

    return body


def app_kid(f):  # writes its argument count as one digit, then exits with 7
    f.i32(DIG)
    f.call("argc")
    f.i32(48)
    f.add()
    f.st32()
    f.write(1, DIG, 1)
    f.i32(7)
    f.call("exit")


def app_sleeper(f):  # sleeps 10 s unless it is stopped
    f.i32(10000)
    f.call("sleep_ms")


def app_guard(f):  # a pid that is not this app's child: both calls are refused
    f.store_call(RES, "child_wait", 12345, VAL, 4, 0)
    f.store_call(RES + 4, "signal", 12345, 2)
    f.write(1, RES, 8)


def app_wait_only(f):  # waits on stdin alone: a stop request ends the app, as it does for a read
    f.store_call(RES, "wait", REC0, 8, -1)
    f.write(1, RES, 4)


def app_resize_keep(f):  # the queue exists but this wait does not list it: a window change is kept for it
    f.i32(REC1)
    f.call("events")
    f.st32()
    f.store_call(RES, "wait", REC0, 8, 400)
    f.store_call(RES + 4, "event_next", EV, 16, 0)
    f.write(1, RES, 8)
    f.write(1, EV, 16)


def app_orphan(f):  # starts a child, lets it finish, reports its pid, and exits without waiting for it
    f.store_call(RES, "spawn", S_KID, S_ARGS, 4, 0)
    f.i32(50)
    f.call("sleep_ms")
    f.write(1, RES, 4)


APPS = {
    "wait_timeout": (app_wait_timeout, []),
    "wait_stdin": (app_wait_stdin, []),
    "wait_bad": (app_wait_bad, []),
    "events_wait": (app_events_wait, []),
    "tty_raw": (app_tty_mode(1), []),
    "tty_nosig": (app_tty_mode(2), []),
    "tty_noecho": (app_tty_mode(4), []),
    "tty_cooked": (app_tty_mode(0), []),
    "wait_only": (app_wait_only, []),
    "resize_keep": (app_resize_keep, []),
    "orphan": (app_orphan, []),
    "tty_get": (app_tty_get, []),
    "term_size": (app_term_size, []),
    "env": (app_env, []),
    "time": (app_time, []),
    "log": (app_log, []),
    "spawn": (app_spawn, []),
    "spawn_term": (app_spawn_signal(2), []),
    "spawn_kill": (app_spawn_signal(4), []),
    "kid": (app_kid, []),
    "sleeper": (app_sleeper, []),
    "guard": (app_guard, []),
}


def c_array(name_, data):
    lines = [f"static const uint8_t {name_}[{len(data)}] = {{"]
    for i in range(0, len(data), 32):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 32]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out = [
        "/* Hand-encoded wasm apps for tests/unit/test_api_events.c and tests/unit/test_api_proc.c, GENERATED by",
        " * tests/unit/tools/gen_core_fixtures.py. Do not edit: change the generator and run it again. */",
        "#ifndef WASM_FIXTURES_CORE_H",
        "#define WASM_FIXTURES_CORE_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for key, (body, _) in APPS.items():
        mod = build(body, STRINGS)
        out.append(c_array("WASM_" + key.upper(), mod))
        out.append("")
    out += ["#endif", ""]
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
