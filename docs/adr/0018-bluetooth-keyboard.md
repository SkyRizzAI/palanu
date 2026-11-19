# ADR-0018: The device as a Bluetooth keyboard

Status: accepted (19 November 2026).

## Context
A handheld with a full keyboard is also a keyboard for a phone, a tablet or a computer in a rack. Flipper and Bruce turn their
devices into Bluetooth keyboards; PalanuOS had the radio and the keys but no way to send one to another machine.

## Decision
- **A portable seam, a platform transport.** `pal/hid.h` is an ops table (start, stop, state, report, peer). The system maps a
  PalanuOS key to a USB HID usage and modifiers in the US layout (`system/src/hid.c`, the layout a host assumes of a keyboard that
  does not say otherwise; a code point with no key on it is not sent). Each key is a press report then a release report: the host
  repeats a held key itself.
- **HID over GATT with NimBLE** (`platform/esp-idf/palanu/hid_ble.c`), written for PalanuOS rather than esp_hid: three services
  (HID with one keyboard input report, Battery, Device Information's PnP ID), about 250 lines. Just Works pairing with bonding
  (the device has no way to show a code to the host); the bond is kept in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST`), so a paired host
  reconnects after a restart. A host that lost its bond pairs again (repeat pairing deletes the old one).
- **One GATT server at a time.** While the keyboard runs it owns the BLE radio: Palanu Link over BLE stops at start and comes back at
  stop. Wi-Fi is untouched.
- **The Keyboard app** (`apps/hidkbd.c`, id `keyboard`) is the only user: it advertises while it is in front, sends every key it
  gets, and shows whether a host is connected and what went last. Holding the cycle key leaves it, as it leaves every app; leaving
  stops the keyboard. The simulator has a recording transport that "pairs" after 0.8 s and logs each report.

## Consequences
- The firmware is at 84.8% of the update slot (the release warns at 85%): the next large feature needs room made first.
- Proven by `test_hid` (the whole printable range, the modifiers, press and release) and in the simulator (golden images
  `keyboard-waiting`, `keyboard-connected`); the BLE transport builds for hhv2 but has not paired with a real host yet.
- USB HID (the S31's USB device) can reuse the seam later with another transport.
