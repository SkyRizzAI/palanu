# Security policy

## Reporting a vulnerability

Do not open a public issue for a vulnerability. Use this repository's private reporting feature
(GitHub: Security > Report a vulnerability). Include the version (`VERSION`), the board, the steps to reproduce, and the impact.

## Scope

PalanuOS firmware (nema, system, apps), the WebAssembly app runtime and manifest permissions, Palanu Link, the SSH client, OTA
updates and the tools in `forge/`.

## Model in short

- Apps run in a WebAssembly sandbox with manifest permissions. Hardware is reachable only through the API, and a pin or bus is
  visible to apps only when the board marks it as app-safe.
- Palanu Link protects remote access with pairing. OTA updates are health-checked and can roll back.
- User secrets (Wi-Fi passwords, SSH keys) stay on the device and are not sent anywhere else.

Known limits are listed in `docs/RELEASE.md` and `CHANGELOG.md`.
