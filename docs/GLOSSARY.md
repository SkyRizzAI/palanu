# Glossary

Only four special names: **PalanuOS**, **Nema** (the kernel), **Forge** (the tooling that runs on a computer), **Palanu Link** (the protocol). Everything else is a generic term.

| Term | Meaning |
|---|---|
| Subsystem | A part of the firmware above Nema that is always present: display, input, desktop, storage, radio, link |
| App | Any program that PalanuOS runs |
| Built-in app | An app that is built together with the firmware |
| Installed app | An app loaded at runtime from storage |
| Executable | A file that holds an installed app (the format is set in the executable ADR) |
| Service | A background app without a screen, supervised and able to start automatically |
| Manifest | The description of an app: id, name, version, API version, mode, permissions, memory |
| Process | One running app: id, arguments, handles, exit code |
| Handle | A number that stands for a file, socket, pipe, or TTY owned by a process |
| TTY | The connection between a process and a terminal, in line or raw mode |
| Terminal | A character-grid viewer that understands escape sequences |
| Shell | The `sh` command interpreter, an ordinary process on top of a TTY |
| Session | One TTY together with its shell and jobs |
| Desktop | The compositor, status bar, overlays, and home app |
| Theme | Tokens + palette + component table |
| Component | A drawing function that the system calls through the active theme |
| Channel | A numbered stream inside one link connection |
| Permission | The name of a right that an app requests in its manifest and that is checked on every API call |
| Admission | A check of the remaining internal RAM before a task or radio is started |
