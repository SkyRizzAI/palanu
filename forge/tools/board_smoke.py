"""Smoke and soak test on the real board, over its serial console (./pal smoke).

    python board_smoke.py PORT [--minutes N] [--csv FILE] [--keep]

It resets the board, makes sure it booted normally, then visits every screen the device has (all Settings pages, the home and
launcher skins, the lock screen, Control Center, the app list, notifications, Files, Terminal commands) while it watches the
console for a panic, a stack overflow, an assert or a watchdog reset. Crashes like opening the app list rebooted the board
(a 6 KB array on a 6 KB stack, 2026-11-14) went unnoticed because nothing but a person ever visited those screens.

With --minutes it then keeps going (T8.4, the 24 h endurance test is this with --minutes 1440): every round it visits the
screens again, turns Wi-Fi and Bluetooth on and scans, and samples `mem` into a CSV. The verdict compares the first and last
third of the samples: internal RAM that keeps falling is a leak. Exit status 0 only when nothing crashed and memory held.

Run it inside the ESP-IDF python environment (it provides pyserial). It never types a Wi-Fi password: Wi-Fi is only turned
on (the saved network, if any, reconnects by itself).
"""
import re
import sys
import time

import serial

PROMPT = "palanu:"
PANIC = re.compile(r"Guru Meditation|panic'ed|Stack protection|abort\(\)|assert failed|Task watchdog|Brownout|"
                   r"rst:0x(?:7|8|9|c|d|e)\b|CORRUPT HEAP|heap: DAMAGED|Backtrace:|LoadProhibited|StoreProhibited")
MEM = re.compile(r"^internal\s+(\d+)K\s+(\d+)K\s+(\d+)K\s+(\d+)K", re.M)
EXTERNAL = re.compile(r"^external\s+(\d+)K\s+(\d+)K\s+(\d+)K\s+(\d+)K", re.M)

SETTINGS_PAGES = (
    "display appearance desktop-setting controls keys keymap wifi wifi-detail wifi-scan bluetooth remote remote-link "
    "link-clients device sound led battery camera touch sensors secure notifications notify-apps power storage "
    "storage-detail apps app-detail app-terminal services service-detail datetime profile pin updates developer about "
    "about-detail about-notices"
).split()

SCREENS = (
    ["uishow home", "uishow launcher strip", "uishow launcher grid", "uishow launcher compact", "uishow launcher list",
     "uishow applist", "uishow lock", "uishow control", "uishow notifications", "uishow notifications empty",
     "uishow gallery", "uishow splash 300", "uishow screen toast", "uishow screen popup", "uishow screen permission", "uishow inputmenu caps sym ctrl", "uishow inputmenu",
     "files /", "files /home", "files /data", "files /apps"]
    + [f"uishow settings {p}" for p in SETTINGS_PAGES]
)
SHELL = ["ls /", "ls /bin", "free", "ps", "svc", "radio", "mem", "date", "sd", "heapcheck", "help"]


class Board:
    def __init__(self, port):
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, 115200, 0.1
        self.p.dtr = False
        self.p.rts = False
        self.p.open()
        self.log = ""
        self.mark = 0

    def reset(self):
        self.p.rts = True
        time.sleep(0.2)
        self.p.rts = False

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            d = self.p.read(4096)
            if d:
                self.log += d.decode("utf-8", "replace")

    def wait_for(self, text, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.2)
            if text in re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", self.log[self.mark:]):
                return True
        return False

    def run(self, cmd, wait=1.2):
        """Sends a line, waits, returns the console output since (panics are looked for by the caller)."""
        start = len(self.log)
        self.p.write((cmd + "\n").encode())
        self.pump(wait)
        return self.log[start:]

    def new_events(self):
        out = self.log[self.mark:]
        self.mark = len(self.log)
        return out


def clean(s):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def main() -> int:
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    port = a[0]
    minutes = float(a[a.index("--minutes") + 1]) if "--minutes" in a else 0.0
    csv = a[a.index("--csv") + 1] if "--csv" in a else None
    b = Board(port)
    problems = []

    def check(where):
        out = clean(b.new_events())
        m = PANIC.search(out)
        if m:
            problems.append(f"{where}: {m.group(0)}")
            print(f"  !! {where}: {m.group(0)}")
            return False
        return True

    print("resetting the board…")
    b.reset()
    if not b.wait_for(PROMPT, 25):
        print("no shell prompt after 25 s: the board did not boot")
        return 1
    b.pump(2.5)
    out = clean(b.log)
    if "SAFE MODE" in out:
        print("the board booted in safe mode (earlier resets did not reach ready): clearing it and booting again")
        b.run("safemode off")
        b.log = ""
        b.mark = 0
        b.reset()
        if not b.wait_for(PROMPT, 25):
            print("no prompt after the second boot")
            return 1
        b.pump(2.5)
    b.new_events()
    boots = len(re.findall(r"rst:0x", clean(b.log)))

    samples = []

    def sample(tag):
        text = clean(b.run("mem", 1.4))
        m, e = MEM.search(text), EXTERNAL.search(text)
        if m:
            row = (time.time(), tag, int(m.group(2)), int(m.group(3)), int(m.group(4)), int(e.group(2)) if e else 0)
            samples.append(row)
            return row
        return None

    first = sample("boot")
    print(f"boot ok; internal RAM free {first[2]}K, minimum {first[3]}K, block {first[4]}K" if first else "boot ok (no mem line)")

    def visit_all():
        for cmd in SCREENS:
            b.run(cmd, 0.9)
            if not check(cmd):
                return False
            b.run("", 0.1)
        for cmd in SHELL:
            b.run(cmd, 0.9)
            if not check(cmd):
                return False
        b.run("uishow home", 0.9)  # leave the board on home, never on a fixture or a settings page
        return True

    print(f"visiting {len(SCREENS)} screens and {len(SHELL)} commands…")
    ok = visit_all()
    row = sample("after tour")
    if row:
        print(f"after the tour: free {row[2]}K, minimum {row[3]}K")
    if not ok:
        print("CRASH during the tour: see above (the board rebooted, so the rest was skipped)")
        return 1

    if minutes > 0:
        end = time.time() + minutes * 60
        rnd = 0
        print(f"soak for {minutes:g} min: screens, Wi-Fi and Bluetooth on, scans, memory samples…")
        b.run("wifi on", 6)
        b.run("ble on", 4)
        while time.time() < end:
            rnd += 1
            if not visit_all():
                break
            b.run("wifi scan", 8)
            check("wifi scan")
            b.run("svc restart link-ws", 3)
            check("svc restart link-ws")
            r = sample(f"round {rnd}")
            if r:
                print(f"  round {rnd:3d}: free {r[2]}K minimum {r[3]}K block {r[4]}K psram {r[5]}K")
            if len(re.findall(r"rst:0x", clean(b.log))) > boots:
                problems.append(f"round {rnd}: the board reset")
                break
        if csv:
            with open(csv, "w") as f:
                f.write("time,tag,internal_free_kb,internal_min_kb,internal_block_kb,psram_free_kb\n")
                for s in samples:
                    f.write(f"{s[0]:.0f},{s[1]},{s[2]},{s[3]},{s[4]},{s[5]}\n")
            print(f"samples in {csv}")

    # the verdict on memory: the last third against the first third of the samples
    verdict = []
    if len(samples) >= 6:
        third = len(samples) // 3
        head, tail = samples[:third], samples[-third:]
        h = sum(s[2] for s in head) / len(head)
        t = sum(s[2] for s in tail) / len(tail)
        hm = min(s[3] for s in head)
        tm = min(s[3] for s in tail)
        print(f"internal free: first third {h:.0f}K, last third {t:.0f}K; minimum {hm}K then {tm}K")
        if t < h - 6:
            verdict.append(f"internal RAM free fell {h - t:.0f}K over the run: a leak")
        if tm < 40:
            verdict.append(f"the minimum internal RAM fell to {tm}K (the floor is 32K)")
    problems += verdict
    if problems:
        print("\nFAIL")
        for p in problems:
            print("  -", p)
        return 1
    print("\nOK: no crash, no reset, memory held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
