# ADR-0022: Versions from Conventional Commits, releases by a merged PR, Web Forge and the simulators on GitHub Pages

Status: accepted (20 November 2026).

## Context
The owner wants Web Forge on GitHub Pages, built from the repository, with the simulator of each release loadable from that page,
and later the board's firmware too. Releases were made by hand (`./pal release --version X`, a heuristic changelog, a local tag);
nothing was published. A test (20 November 2026, an `Origin` header from a github.io page) showed that a page cannot download
GitHub release files: `github.com/.../releases/download/...` redirects to `release-assets.githubusercontent.com`, and neither
answer carries `Access-Control-Allow-Origin`; the API's asset URL redirects to the same host. Only the API's JSON (`api.github.com`)
and raw repository files allow CORS.

## Decision
- **The version comes from the commits** (`forge/tools/version.ts`, the repo playbook's rule that a version is never typed by
  hand). While PalanuOS is 0.x: a breaking change ("!" or a `BREAKING CHANGE:` footer) or a `feat` raises the minor, `fix`, `perf`
  or `revert` the patch; docs, test, ci, build, chore, refactor and style alone release nothing. From 1.0 a breaking change raises
  the major. A prerelease (`X.Y.Z-rc.N`) is finished by the next release. The commits counted start at the newest `v*` tag, or,
  before the first, at the commit that last wrote `VERSION`.
- **The changelog is grouped from the same commits** (Breaking changes, Features, Fixes, Performance, Reverts; "scope: summary").
  A section already in `CHANGELOG.md` for that version is kept, so a release can be curated by editing it.
- **A release is a merged PR** (`.github/workflows/release.yml`, the release-please model without its dependency): every push to
  main opens or updates "chore(release): vX.Y.Z" with `VERSION`, `version.h` and `CHANGELOG.md`. Merging it is the owner's
  decision to release (AGENTS.md: no tag without the owner). The push that merges it runs the gates, builds the simulator, tags
  `vX.Y.Z` and publishes the GitHub Release. `./pal release` stays for a release from a computer with the board gates and uses the
  same rules.
- **Pages holds everything the page loads, on one origin** (`.github/workflows/pages.yml`, `forge/web/site.ts`): Web Forge, the
  catalogue, the simulator built from main (`sim/dev/`), each of the last ten releases' simulators (`sim/<version>/`, copied
  from the release assets by the workflow, where CORS does not apply), and `firmware.json` listing them. The release workflow
  calls the Pages workflow once its release exists (the built-in token's events start no other workflow). Pages deploys from
  Actions, not a branch.
- **The page picks a build.** Web Forge reads `firmware.json` and offers each release and dev (the idle screen's Sim firmware,
  Simulator > Firmware); the simulator's files in IndexedDB stay across builds, as a board's data stays across an update. Without
  the manifest (`./pal web`, a file), the build next to the page is used.
- **Unsigned for now.** Releases and builds say so (`signed: false` in the manifest, an UNSIGNED tag in the page, a line in the
  release notes) until the release key and secure boot are decided (docs/RELEASE.md section 7).

## Consequences
- The board's firmware is not built by CI yet (ESP-IDF for the S31 in CI is unproven); it is the next step, and the same Pages
  copy gives the page the image to flash over Web Serial or to send by OTA.
- GitHub Pages cannot send the isolation headers the simulator needs: `coi-sw.js` adds them, and the first start reloads the page
  once. The built-in browser of the development app does not run service workers, so that step was checked by reasoning and by
  serving the site with the headers, not end to end.
- Wi-Fi/LAN remote does not work from an https page (`ws://` is mixed content); USB and Bluetooth do.
- The repository settings need two switches: Pages source "GitHub Actions", and "Allow GitHub Actions to create and approve pull
  requests". A release PR opened by the built-in token does not run CI on itself; main's CI runs after the merge.
- Pages sites are limited to about 1 GB; ten simulators are about 30 MB.
