# ADR-0015: An app catalogue on any web server; install and manage a board from the browser

Status: accepted (19 November 2026).

## Context
Every competitor with a community has a catalogue (Flipper's, MicroHydra's, AkiraOS's, SolarOS's packages) and a first install that
needs no toolchain (Bruce's and Meshtastic's web flashers). PalanuOS installed apps only through Palanu Link or `app install`, the
first install needed ESP-IDF, and Web Forge's Files tab could not delete, rename or install.

## Decision
- **The catalogue is a text list on any web server** (`docs/STORE.md`): one line per package with its SHA-256. No server code, no
  account: a GitHub folder is enough, and the device's default is the project's `store/`. The device fetches it over HTTPS
  (ADR-0013), checks each download against the list, and installs through the same path as `app install`, so the signature policy
  and the permissions at first use still decide. Store is a form app (two looks, ADR-0012); `store` is its command.
  `forge/tools/store.ts` builds a catalogue from projects.
- **Install from the browser.** Web Forge gains an Install page (`/flash/`) that writes the bootloader, partition table and system
  over Web Serial with esptool-js (0.7.1, which supports the ESP32-S31), from the files and offsets of the last `./pal build`. No
  ESP-IDF on the user's computer.
- **Files from the browser.** Palanu Link's fs channel gains REMOVE, MKDIR, RENAME and INSTALL (docs/PLP.md); Web Forge's Files tab
  uses them, with an editor for text files.

## Consequences
- The packages in `store/` are unsigned until a release key exists: a device installs them in developer mode.
- Proven in the native simulator against a local copy of the catalogue (`store list`, `store install`, the Store app under Athena) and
  by `test_store`; the Install page loads and reads the image in a Chromium browser, but has not written a real board yet.
- The bun tests of Palanu Link have four failures that were already there before this change (screen mirror colours, many
  connections, remote key, packages in /apps); they are listed in docs/STATE.md to be fixed.
