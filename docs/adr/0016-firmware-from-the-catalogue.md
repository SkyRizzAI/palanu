# ADR-0016: The system from the catalogue, and a signed catalogue

Status: accepted (19 November 2026).

## Context
A device could update its firmware only from a computer (Palanu Link: `palanu ota`, Web Forge). Every phone, and the competitors
with an app store, update from the device itself. The catalogue (ADR-0015) already reaches the device over HTTPS and checks each
download against the SHA-256 in its list, but nothing proved the list itself came from the project, and a firmware image runs with
every right, so it needs more than a checksum.

## Decision
- **The firmware is a line of the catalogue.** An entry whose id is `firmware:<board>` (the board's `pal_board_info()->name`) is
  that board's image (the raw app image `./pal build` writes), with its version, size, SHA-256 and path. The app list leaves those
  lines out, so an older reader sees only apps. One list serves every board.
- **The list can be signed.** `index.txt.sig` next to it holds 64 bytes: Ed25519 over the list's SHA-256, the same scheme as
  package signatures. The device checks it with the keys it already trusts for packages: `/etc/trust` and the release keys
  compiled in (`system/src/release_keys.h`).
- **A firmware update needs a signed list, or developer mode.** An unsigned list still serves apps (each package goes through the
  package policy as before), but the system image only comes from a list a trusted key signed, unless the owner turned developer
  mode on.
- **Streamed into the other slot.** `pal_store_update_firmware` writes the download straight into the inactive OTA slot
  (`pal_ota`) while it hashes it; a download larger than the list says is stopped, and only an image with the list's size and
  SHA-256 is handed to `end()`, which makes it the next boot. Anything else aborts the slot. The running system is never touched,
  and the bootloader's rollback (F8) still covers an image that does not confirm itself.
- **On the device:** `update` (`check`, `install [--reboot]`) and Settings > Developer > Updates (Check for update, then Install
  update). **In the tools:** `store.ts --firmware BOARD=FILE[@URL] --sign KEY`; `./pal release` adds the new image (at its GitHub
  release download) to `store/index.txt` and signs it when `PALANU_RELEASE_KEY` names the key file.
- **The release key is the owner's.** No private key is made or kept in the repository: `palanu keygen release.key` prints the
  public key and the line for `release_keys.h`; the owner keeps the `.key` offline.

## Consequences
- Until the owner makes the release key, the project's catalogue is unsigned: a device updates its system from it only in developer
  mode (on by default in a developer build, off in a release build).
- Proven by `test_store_fw` (a local server and a pretend slot: a signed list installs, a wrong checksum aborts, an unsigned list
  needs developer mode, a signature by another key is not trusted) and in the native simulator against a local catalogue signed by
  `store.ts` (the list reads as signed once its key is in `/etc/trust`; the simulator has no update slot, so install says so). The
  download into a real slot has not run on hhv2 yet.
