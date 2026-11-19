# The app catalogue

The Store app (and the `store` command) lists the packages of a catalogue, installs them and updates them. A catalogue is a folder on
any web server: a list, `index.txt`, and the `.papp` files it names.

## On the device
- **Store** in the launcher: Refresh reads the catalogue; an app's page shows its summary, version, the installed version and Install,
  Update or Install again. It is a form, so each desktop draws it in its own look.
- `store` (or `store list`) reads the catalogue and lists it; `store info ID`, `store install ID`, `store update` (every installed app
  with a newer version).
- The catalogue's URL is the first line of `/etc/store.url` (edit it with the Editor); without it the device reads the project's
  catalogue: `https://raw.githubusercontent.com/SkyRizzAI/palanu/main/store/index.txt`.
- A download is checked against the SHA-256 the list gives, then installed as `app install` installs: the trust policy applies (a
  trusted signature, or developer mode), and the permissions are asked at first use (ADR-0014).

## The list
```
palanu-store 1
id<TAB>version<TAB>name<TAB>size<TAB>sha256<TAB>path<TAB>summary
```
`path` is relative to the list (or a full URL). A line that does not parse is skipped.

A line whose id is `firmware:<board>` (`firmware:hhv2`) is PalanuOS for that board, not an app: Store and `store list` leave it
out, and `update` reads it (ADR-0016).

## Signing
`index.txt.sig` next to the list holds 64 bytes: an Ed25519 signature over the list's SHA-256. The device trusts it when the key
is in `/etc/trust` (one hex public key per file) or compiled in as a release key (`system/src/release_keys.h`). An unsigned list
still serves apps; the system image only comes from a signed list, or in developer mode.

## The system
- `update` (or `update check`) reads the catalogue and compares its firmware for this board with the running version;
  `update install [--reboot]` downloads it straight into the other update slot, checking its size and SHA-256 as it streams, and
  only a matching image starts at the next boot.
- Settings > Developer > Updates: Check for update, then Install update.

## Making a catalogue
`bun forge/tools/store.ts build [DIR]` builds the projects listed in `forge/tools/store.ts` into `DIR` (default `store/`) and writes
`index.txt`; `bun forge/tools/store.ts index DIR` writes only the list for packages already there (keeping the summaries it had).
Push `store/` and devices see it. Options for both:
- `--firmware BOARD=FILE[@URL]`: a firmware line for PalanuOS (the version in `system/include/pal/version.h`); the device downloads
  `URL`, or `FILE` copied next to the list when no URL is given.
- `--sign KEYFILE`: writes `index.txt.sig` with a key from `bun forge/tools/palanu.ts keygen`.

`./pal release` does this for hhv2: the new image at its GitHub release download, signed when `PALANU_RELEASE_KEY` names the key.
