# PalanuOS user guide

For the hhv2 board: 320x240 screen, 39-key keyboard. The guide to building apps is in `DEVELOPER.md`, to moving the system to
another board in `PORTING-A-BOARD.md`, and to controlling the device from a computer in `WEB-FORGE.md`.

## 1. Powering on and moving around

At power-on PalanuOS shows the start screen, then **home**: clock, date, animated wallpaper, and status badges (Wi-Fi,
Bluetooth, remote link, battery, notification count). The bar at the bottom of the screen (the **legend bar**) always shows the keys
that apply on that screen, so there is nothing to memorize.

| Key | Function |
|---|---|
| `Fn` + `W` `A` `S` `D` | arrow up, left, down, right |
| `Enter` | select, open, press |
| `Esc` | back |
| `Tab` | extra commands on that screen (in notifications: clear all) |
| cycle key ⟳: tap | switch to the previous app |
| cycle key ⟳: hold | return to home |
| `Nm` | the symbol printed on a key (hold, or tap and then press one character) |
| `Fn` double tap | input menu: on-screen keyboard, symbol lock, digits, Caps Lock, Ctrl. In that menu one tap of Fn moves the selection down, `Enter` selects (or type the letter on its row), Fn twice or `Esc` closes it |
| `Shift` double tap | Caps Lock on or off |

On home: `Enter` opens the app list, `Fn+W` opens the **Control Center** (dark mode, Wi-Fi, lock, Settings, restart, brightness, volume), `Fn+S`
opens the **notifications**. The full key list is in `KEYMAP.md`.

## 2. Settings

Settings is in the app list. Four groups: **Screen** (Display, Appearances, Controls), **Connections** (Wi-Fi, Bluetooth,
Remote), **Device** (hardware, notifications, storage, apps), **System** (profile and PIN, date and time, updates,
developer, About).

- **Display > UI Scale**: 1x to 2.5x. 1x shows the most content, 2x is the default size, 2.5x is the largest.
- **Display > Terminal Font**: Small (64 columns), Medium (53), Large (40). Choose Small for programs that print wide lines.
- **Profile > PIN**: lock the screen with a PIN of 4 to 8 digits. The screen locks after the time set in Display.
- Rows that are grayed out or unavailable can still be selected: press `Enter` and the screen states the reason.
- Text that is too long for one row scrolls by itself while its row is selected.

## 3. Networking

**Wi-Fi**: Settings > Wi-Fi, turn it on, pick a network under *Other networks*, and type the password with the keyboard. A network that connected
successfully is saved (up to 8, the most recent first) and joins by itself next time; when one is out of reach the next saved one is
tried. Settings > Wi-Fi > *My networks* shows them: each has Auto-Join, a manual IPv4 address (address, subnet mask, router, DNS)
instead of DHCP, and Forget. From the terminal: `wifi scan`, `wifi connect SSID PASSWORD`, `wifi saved`, `wifi forget SSID`,
`wifi auto SSID on|off`, `wifi ip SSID ADDRESS MASK ROUTER [DNS]` or `wifi ip SSID dhcp`.

**Internet tools**: `curl` fetches http:// and https:// (`-i` headers, `-I` headers only, `-X` method, `-H` a header, `-d` a body,
`-L` follow redirects, `-o` save to a file, `-k` accept any certificate). `ping HOST` and `tcping HOST PORT` check that a host
answers and how long it takes.

**Bluetooth**: Settings > Bluetooth. The device advertises itself as **PalanuOS** so that a computer (Web Forge) can connect.

Wi-Fi and Bluetooth can be on at the same time. If internal memory is too low to turn either one on, the device
refuses with a message that states what it needs (see `RADIO.md`).

## 4. Controlling the device from a computer (Remote)

Settings > Remote shows the device address (`192.168.x.x:7001`) and the **Remote Enabled** switch. To pair a computer
once:

1. On the device: Settings > Remote > **Pair a Device**. A 6-digit code appears that is valid for 2 minutes and allows 3 attempts.
2. On the computer: open **Web Forge** (`./pal web`, then http://localhost:5174 in Chrome or Edge), choose USB, Bluetooth, or
   Wi-Fi/LAN, and type the code.

After that the computer signs in by itself with the stored token. A USB cable does not need pairing. **Authorized Devices** lists
the paired computers, and **Log Out All Devices** revokes all of them. Data after sign-in is not yet encrypted: use a network
you trust.

## 5. Files and the microSD card

**Files** browses `/home`, `/data`, `/apps`, `/tmp`, and `/sd`. The microSD card is mounted at boot on `/sd`. The slot has no
card-detect switch, so a card inserted after boot is mounted through Settings > Storage > **Mount SD Card** or `sd mount`.
Remove the card only after **Eject SD Card** (or `sd eject`).

## 6. Terminal and SSH

**Terminal** is a shell. `Alt+N` opens a new tab, `Alt+W` closes it, `Alt+Tab` switches tabs, `Shift+Fn+W` / `Shift+Fn+S` scroll the screen history one line, `Shift+Fn+R` / `Shift+Fn+F` one screen.
Arrow up repeats the previous command, **also after a restart** (history is kept in `/data/sh/history`; start a command with a
space so it is not remembered).

SSH: `ssh user@host`. The first time you connect to a host, the device shows its fingerprint and asks `yes/no`; a `yes` answer
is stored in `/data/ssh/known_hosts`. `Ctrl-D` or `exit` ends the session; `~.` at the start of a line disconnects immediately. For key
login: `ssh-keygen`, then add the line it prints to `~/.ssh/authorized_keys` on the server.

Characters outside Latin (Hebrew, Arabic, Thai, Devanagari, symbols, emoji, Nerd Font icons for Powerline prompts) are drawn from the
fallback font; CJK ideographs and Hangul need the file `/data/unifont-cjk.pfnt` (`docs/FONTS.md`), without which they appear as empty boxes.

## 7. Updating the firmware

From a computer: Web Forge > Device > **Firmware update**, or `palanu ota palanuos.bin`. The device writes the image to the other slot,
verifies it, then restarts into it. The new image must confirm itself within 10 seconds after boot; otherwise the
bootloader returns to the old version. A corrupt image is rejected before the slot is replaced. Settings > Updates shows the version, the running
slot, and whether updates over the link are supported.

## 8. When something goes wrong

| Symptom | What to do |
|---|---|
| The screen shows "safe mode" or an installed app does not appear | three boots in a row did not finish in time (30 seconds), so only core services are running. Type `safemode off` in the terminal, then restart |
| Wi-Fi refuses to turn on | not enough internal memory: close heavy apps, or turn Bluetooth off first. `radio` in the terminal shows the numbers |
| A `.papp` app is rejected on install | release builds reject unsigned packages: turn on Settings > Developer > Developer mode, or put your public key in `/etc/trust/` (`PAPP.md`) |
| The device does not respond from the computer | `Remote Enabled` is off, or the computer is on another network. Over a USB cable it always works |
| You want to reinstall from scratch | connect USB and run `./pal flash` |
