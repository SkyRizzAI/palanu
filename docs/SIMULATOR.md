# Simulator

PalanuOS runs without a board in two places. Both run the same portable code (`nema/`, `system/`, `apps/`); only the board around it is simulated.

| | Native (`./pal sim`) | Browser (`./pal web`, Start simulator in Web Forge) |
| --- | --- | --- |
| Where | a window (SDL2) on your computer | a tab, nothing to install for the user |
| Build | CMake, `platform/host/sim_main.c` | Emscripten, `platform/web/web_main.c` |
| Used for | golden images, unit and scripted tests, development | trying the OS, demos, hardware set-up without a device, teaching |
| Hardware | flags: `--leds N --sensors N --gpios N --i2c --battery P --lora PORT:PEER` (a LoRa module over UDP: two simulators with crossed ports talk) | the page's Simulator section; the same code (`platform/host/sim_hw.c`) |

## Browser simulator

```sh
./pal web setup      # once: installs Emscripten into refs/emsdk (git, Python 3.10 or newer)
./pal web            # builds the system for the browser and Web Forge, serves http://localhost:5174 (Start simulator)
./pal web --no-sim   # Web Forge only (no Emscripten needed)
```

The page needs cross-origin isolation (the `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`
headers), because each Nema task runs on a Web Worker that shares the system's memory. `forge/web/serve.ts` sets them on every response.
Any other host must send the same two headers; on a static host that cannot (GitHub Pages), the page registers `coi-sw.js`, a service worker that adds them, and reloads once (https or localhost only). Chrome, Edge, Firefox and Safari work; a computer is recommended.

### What runs in the page

The simulator is one of the two devices of Web Forge (`docs/WEB-FORGE.md`, ADR-0021): the idle screen's **Start simulator** boots the
system (`palanu-web.wasm`, about 1.8 MB) in the page, and it draws its screen to the same canvas a board's mirror uses. The page reaches
the system's Palanu Link through a **virtual cable** (`pal_web_link_*` in `platform/web/web_main.c`: two memory rings and a `vlink`
task, trusted like a USB cable), so the Terminal, Files, System and Store sections work exactly as for a board. The saved data
(`/data`: settings, files, installed apps) lives in IndexedDB and survives a reload; System > **Erase simulator data** starts from a
first boot. **Restart** reloads the system with its files; **Shutdown** stops it. WebAssembly apps (`.papp`) run too: the system
embeds the same WAMR interpreter as the board.

### The Simulator section

Settings grouped like the device's own Settings, a list on the left and one pane:

| Group | What you can change |
| --- | --- |
| **Screen** | the panel's resolution (presets or WIDTHxHEIGHT, 64x48 up to 320x240, applied at restart; the system lays itself out for it), size (fit or 1x to 4x), brightness, smooth or crisp pixels, pixel grid, grayscale, inverted colours, the board frame |
| **Wi-Fi** | the radio and link state, the device's address, the networks a scan finds as cards (name, signal with bars, channel, secure, password), "every connect fails", dropping the link |
| **Internet** | the network bridge: Direct, Proxy (with its URL) or Off |
| **Bluetooth** | the radio's state |
| **Power** | the battery level (a battery picture) or "no battery" |
| **Hardware** | how many LEDs, sensors and pins and whether there is an I2C bus (applied at restart); live LED colours; sensor readings; pin levels in both directions; I2C device addresses |
| **Input** | send keys even when the screen has no focus; sticky modifiers; a line typed on the device; **hotkeys** (record any combination, run any step); **key sequences** (write, save, repeat, record, stop) |

The log is the console's **Logs** tab (timestamps, filter, problems only, wrap, pause, clear, save). Frame rate and uptime are in System.
Everything is remembered in this browser (`localStorage`, key `palanu.sim.config`). Hardware counts and the Wi-Fi list are re-applied at every start.

### Keyboard

Click the screen and type. Letters, Enter, Esc, Tab, arrows, Home, End, Page Up and Down, Delete go to the device; F1 is the cycle key's tap
(switch), F2 its long press (home); Ctrl and Alt with a letter send a chord. The picture of the hhv2 keyboard presses the physical matrix, so
Fn, Nm, Shift and the long press of the cycle key behave exactly as on the board: hold Fn (or click it with sticky modifiers on) and tap
W A S D for the arrows.

### Key sequences and hotkeys

One step per line, the same words as the native simulator's flags:

```
cmd LINE         type LINE and press Enter       type TEXT        type TEXT
key NAME...      enter esc tab backspace up down left right home end pgup pgdn delete
                 switch home-app vkb fntap, ctrl-alt-esc (the system console), ctrl-X, alt-X
wait MS          pause                           tap ROW COL      press and release a hardware key
battery PCT      charge (-1: no battery)         sensor I DEG     sensor I reads DEG degrees
gpio I 0|1       drive pin I                     shot             save a screenshot
restart          restart the system              note TEXT        put a line in the log
```

A hotkey runs one step. Defaults: Ctrl+Shift+S screenshot, Ctrl+Shift+R restart, Alt+H home, Alt+W switch, Alt+K on-screen keyboard.

### Wi-Fi and the network bridge

The simulated Wi-Fi follows the Wi-Fi settings: `wifi scan` lists the networks you set; a secure network with a password refuses any other
password (an empty password accepts all); connecting gives the device the address you set.

A browser cannot open raw sockets, so when the device connects to `host:port` the page opens a WebSocket instead:

- **Direct**: `ws://host:port`. Works for targets that speak WebSocket.
- **Proxy**: the page opens the proxy URL you set (`{host}` and `{port}` are replaced), and the proxy forwards raw TCP. `./pal web` serves one
  at `ws://localhost:5174/tcp/{host}/{port}`: `nc`, `curl` and `ssh` on the device then reach real machines. It accepts only pages of its own
  origin and listens on this computer only.
- **Off**: every connect is refused.

### Bluetooth

The simulated Bluetooth LE radio is present and follows the system's radio manager (`ble on`, Settings, services such as `link-ble`); the panel
shows its state (Simulator > Bluetooth). It has no air interface: a real peripheral cannot be bridged from a browser tab.

### Limits

- The page and the system share one browser tab: a heavy app slows the page, and the tab pauses its workers in the background.
- Timing is not the board's. Use the board for performance numbers (`./pal hwtest`).
- Sockets go through the bridge only; there is no LAN discovery and no inbound connection.
- The Palanu Link servers (TCP, WebSocket) do not listen in the browser: the page itself reaches the system through the virtual cable; for
  another client, use the native simulator (`./pal sim --ws 7100`) and Web Forge's Connect remote.
- One device per tab: the simulator and a board are not open at the same time.

## Code

- `system/src/athena/` and `third_party/lvgl`: the LVGL desktop (also in the firmware); try `desktop use athena` or `desktop use console` in the Terminal (docs/DESKTOPS.md). Both simulators take full-colour images (`present_rgb565`).
- `platform/host/sim_hw.c`: simulated devices and Wi-Fi shared by both simulators; `tests/unit/test_sim_hw.c`.
- `platform/web/`: the Emscripten target (`web_main.c` exports the page's controls, `CMakeLists.txt` the link options and the reasons for them).
- `forge/web/src/sim/`: the simulator in the page (TypeScript): `sim.ts` (start, the virtual cable, the settings), `engine.ts` (the module and
  the bridge), `keymap.ts` (the keyboard picture), `script.ts` (sequences, tested by `script.test.ts`), `config.ts` (settings).
  `forge/web/src/main.ts` is the page around it.
- `forge/web/serve.ts`: the headers, the catalogue and the TCP proxy.
