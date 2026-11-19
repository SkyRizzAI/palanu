# PalanuOS release

The current version is in `VERSION` (and `system/include/pal/version.h`, `CHANGELOG.md`; `./pal check` fails when the three differ).
0.9.0 is a **release candidate**, not 1.0: the gates that need a long run or other hardware are still open (section 3).

## 1. Making a release

```
./pal release --version 0.9.1 --board /dev/cu.usbserial-10      # real release: all gates, board attached
./pal release --bump patch --dry-run --skip-golden               # rehearsal: no files, no commit
```

The gates, in order, stopping at the first failure: (1) git tree clean on `main`; (2) `./pal check`; (3) host tests and golden
images; (4) the **release** build (`PAL_RELEASE=1`), the internal RAM budget (`forge/tools/ramcheck.ts`), and the image size against the 4 MB
OTA slot (warning at 85%, failure at 95%); (5) with `--board`: the release image is flashed to the board and then the smoke tour runs (section 4).

The output: `VERSION`, `version.h` and `CHANGELOG.md` (a new section from the commit subjects since the last tag), `dist/release/`
(`palanuos-X-hhv2.bin`, `web-forge-X.html`, `SHA256SUMS-X`, `release-X.json`), one "Release X" commit, and the tag `vX`.
**Nothing is pushed or published**: that decision is yours. The update package is that `.bin` (`palanu ota`, Web Forge >
Device > Firmware update, or `./pal flash`).

**The release build differs from the developer build in one way**: developer mode is off by default, so an unsigned `.papp`
package is rejected until the owner turns it on in Settings > Developer or puts a public key in `/etc/trust/`
(`PAPP.md`). Built-in apps (shell, SSH) are not affected. Without `PAL_RELEASE=1`, it is on by default so that `palanu deploy` works on a
new board.

## 2. A new board, without touching the device keyboard

A new board connects over USB (the cable is trusted, no pairing), and then everything is done from the computer:

```
palanu --serial /dev/cu.usbserial-10 info
PALANU_WIFI_PASS=... palanu --serial /dev/cu.usbserial-10 wifi set NETWORK-NAME    # or: --pass-stdin
palanu --serial /dev/cu.usbserial-10 run 'wifi'            # one command, the output and exit code come back to the computer
palanu --serial /dev/cu.usbserial-10 svc list
palanu --serial /dev/cu.usbserial-10 logs
```

The Wi-Fi password is never an argument (so that it does not end up in shell history): use `--pass-stdin` or `PALANU_WIFI_PASS`. The SSID
and password must have no spaces or shell characters; otherwise type `wifi connect` on the device. Once connected, `link pair` on the
device and Web Forge or `palanu pair --tcp ADDRESS:7000` pair the computer; the cable can then be unplugged.

## 3. Gate status

Date: 2026-11-14. "Proven" means it was run and the result was observed.

| Gate | Status | Evidence or what is missing |
|---|---|---|
| Host tests (69) and golden images (166 scenes x 2 themes) | proven | `./pal test`, `./pal golden check` |
| Code rules, API ledger, derived files | proven | `./pal check` (also checks the version) |
| Internal RAM: Wi-Fi and Bluetooth together | proven | 120 KB free, 80 KB minimum while both are on; `ramcheck` in the build |
| Stack frame of every function | proven | `ramcheck` (limit 1.5 KB; shell commands 4 KB) |
| Every screen on the board without a crash | proven | `./pal smoke`: 58 screens and 11 commands |
| Boot without a crash with both radios on | **partial** | see section 5 |
| Web Forge over Wi-Fi to the board | proven | pairing, sign-in again with a token, screen mirror (board); all features (simulator) |
| Web Forge over USB and Bluetooth in a browser | **not yet** | the code exists; not yet tried in Chrome with a device |
| SSH to a real server | proven by you | a `root@pve` session in your screenshot; echo, Ctrl-C and Ctrl-D were fixed afterwards and need to be tried again |
| microSD | proven | mounted at boot, 60 mount/eject cycles while the screen was drawing |
| OTA: protocol, health check, rollback | **partial** | the post-boot health check has a test (`test_ota_confirm`); the image transfer over Link has **no** automated test yet, has not been run from an old version to a new one on the board, and rollback of a crashing image has not been tried |
| 24-hour endurance test (T8.4) | **not run** | the tool is ready: `./pal smoke --minutes 1440 --csv endurance.csv`; needs a board attached for a full day |
| Second board (T8.2) and headless target (T8.1) | **not yet** | needs another board from you; the porting steps are in `PORTING-A-BOARD.md` |
| Secure boot and flash encryption (T8.3) | **undecided** | product decision: irreversible once the eFuse is burned |
| Palanu Link data encryption after sign-in | **not yet** | a known limit, `PLP.md` "Threat model" |
| Web simulator (T7.4) | proven in a browser | `./pal web` runs the whole system in a page (docs/SIMULATOR.md); not yet run on a host without the isolation headers |

## 4. Endurance test

```
./pal smoke                                  # one tour: all screens and commands, looking for panics, stack overflows, resets
./pal smoke --minutes 60 --csv hour.csv      # repeat the tour while Wi-Fi and Bluetooth are on, scans, service restarts, `mem` samples
./pal smoke --minutes 1440 --csv day.csv     # 24-hour test
```

The verdict: no crash or reset, internal RAM free in the last third does not drop more than 6 KB below the first
third (that would be a leak), and the minimum does not drop below 40 KB (kernel floor 32 KB). The tool was written after a crash
(a 6 KB array on the desktop task stack when opening the app list) slipped through because nobody visited that screen on the board.

## 5. The rare boot crash

About one boot in sixty ends with an interrupt watchdog reset, nine milliseconds after Wi-Fi and Bluetooth
are turned on in the same second. The address is in the radio driver code (a short wait loop in RAM), so the root cause is in
vendor code and cannot be inspected here. As a precaution, radio start and stop now run one at a time with a
1.2-second gap between different radios (`system/src/radio.c`; no cost in normal use).

Measurement with the new firmware: **60 valid boots with both radios on, 0 crashes**. Measurements before the fix are not valid (the boot
loop went into safe mode after three boots, so the radios did not start; the script is fixed: `safemode off` on every boot,
and a boot that enters safe mode is counted as invalid). One crash in about sixty boots means the base rate is too low
to prove the fix with a few dozen boots, so the status is **partial**: a failed boot is still safe (boot counter, safe mode, then `safemode off`), and the 24-hour test with periodic
restarts is what will measure it properly.

## 6. Known gaps

Features that were checked directly in the code and are confirmed **absent**:

- apps: BadUSB, Wallets, Stopwatch, Clock;
- networking: Bluetooth Discoverable and Pair Request (pairing only goes through Palanu Link); (manual IP, several saved networks with
  Auto-Join, HTTPS and ping were added on 19 November 2026, ADR-0013);
- system: power profile (`power.profile` is only stored), runtime permissions (Allow/Deny), screen `Rotation` (only stored), choice of
  12-hour format (`ui.clock24` is only stored), Wi-Fi monitor/inject for security tools (needs a radio capability
  that does not exist yet).

Three settings that are only stored (**Rotation**, **24-hour clock**, **Power profile**) are now **hidden** from Settings
(`.hidden = true`, with a comment on when to restore them), so that no row does nothing; UI Scale used to be like that
and has since been implemented for real. To make them real: Rotation needs the panel MADCTL and a rotating canvas; 12-hour needs an
AM/PM suffix in the home and lock layouts; the power profile needs a service that applies it.

## 7. What needs a decision or hardware from you

1. A second board for T8.2 and T8.1, so that the "board-agnostic" claim is proven.
2. A board attached for 24 hours for `./pal smoke --minutes 1440`.
3. Secure boot and flash encryption: yes or no, and when (irreversible).
4. Trying Web Forge over USB and Bluetooth in Chrome with a real board.
5. Trying SSH again (echo, Ctrl-C, Ctrl-D) after today's fix.
6. Publishing: `./pal release ...` makes a local tag; pushing and putting `dist/release/` at a download location is your step.
