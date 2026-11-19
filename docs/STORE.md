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

## Making a catalogue
`bun forge/tools/store.ts build [DIR]` builds the projects listed in `forge/tools/store.ts` into `DIR` (default `store/`) and writes
`index.txt`; `bun forge/tools/store.ts index DIR` writes only the list for packages already there. Push `store/` and devices see it.
