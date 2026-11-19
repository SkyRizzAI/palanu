# Radio: Wi-Fi and BLE

This document describes the radio manager (`system/include/pal/radio.h`, `system/src/radio.c`) and how boards use it.
Plan task: T1.5.

## Policy

- At boot, both radios are **off**. Wi-Fi is started only if `radio.wifi` is `on`, or `auto` and stored credentials
  exist. BLE is started only if `radio.ble` is `on`.
- A radio stays on while it has holders. Holders: `svc` (the `radio-wifi` and `radio-ble` services, T3.2) and `link`
  (the `link-ble` service holds BLE while it runs). A second holder does not restart a radio that is already on.
  Radios are started and stopped through services, so `svc list` shows their state (see `svc` below).
- When the last holder leaves, the radio turns off after a grace period (`grace`, default 10 seconds). The grace period prevents
  the radio from going up and down every time use stops briefly. A new request during the grace period cancels it.
  `pal_radio_force_off()` turns it off immediately.
- **Admission**: before starting, the manager checks internal RAM. A radio starts only if:
  - free internal RAM >= cost + 32 KB, and
  - the largest free block >= min(cost / 2, 24 KB).

  If this is not met, the request is rejected with `NEMA_ERR_NOMEM` and a clear message (`pal_radio_last_error()`). The radio
  stays off and the system keeps running.
- The costs used for admission (`RADIO_COST_WIFI_KB`, `RADIO_COST_BLE_KB` in `radio.c`) are still initial values: Wi-Fi
  60 KB and BLE 35 KB. These values are to be replaced with numbers measured on the board (see "How to measure").
- Radio states: `off`, `starting`, `on`, `stopping`, `failed`. A start that fails tears down what it left behind, then the radio
  has state `failed`. The next request tries again.
- Services that need a radio:
  - Palanu Link over TCP (port 7000) is alive only while Wi-Fi has an IP address: the `link-tcp` service with an address gate.
    The address hook in `radio_esp.c` only wakes the supervisor (`psys_svc_poke`).
  - Palanu Link over BLE advertises only while BLE is on.
  - The UART cable and console do not depend on a radio.

## Settings

Stored in `/data/settings` (`key=value` format), read at boot:

| key | values | default | meaning |
|---|---|---|---|
| `radio.wifi` | `auto`, `off`, `on` | `auto` | `auto`: on at boot only if stored credentials exist |
| `radio.ble` | `off`, `on` | `off` | `on`: on at boot |

The commands `wifi on` and `ble on` write `on`, `wifi off` and `ble off` write `off`. A value rejected by admission is still
written, so the next boot tries again.

## Commands

| command | function |
|---|---|
| `radio` | state, holders, cost, details, last error of each radio, and internal RAM (free, minimum, largest block) |
| `wifi on` / `wifi off` | start / stop the `radio-wifi` service (writes `radio.wifi`); `off` turns it off after the grace period |
| `wifi status` | connected or not; when the radio is off it prints `not connected (radio off)` |
| `wifi scan`, `wifi connect SSID [PASS]` | only if the Wi-Fi radio is `on`; otherwise refuses and tells you to run `wifi on` |
| `wifi disconnect`, `wifi forget [SSID]` | do not need the radio on; `forget` with no name erases every saved network |
| `wifi saved` | the saved networks, the most recent first: open or locked, Auto-Join, DHCP or the manual address |
| `wifi auto SSID on\|off`, `wifi ip SSID dhcp`, `wifi ip SSID ADDRESS MASK ROUTER [DNS]` | Auto-Join, and DHCP or a manual address, from the next connect |
| `ble on` / `ble off` | start / stop the `radio-ble` service (writes `radio.ble`); `off` also stops `link-ble` |
| `ble status` | state and whether a client is connected |

## Board implementation (ESP-IDF)

- `platform/esp-idf/palanu/wifi_esp.c`: `pal_wifi_esp_start()` calls `esp_netif_create_default_wifi_sta`, `esp_wifi_init`,
  `esp_wifi_start`. `pal_wifi_esp_stop()` calls `esp_wifi_stop`, `esp_wifi_deinit`, `esp_netif_destroy`, and
  `esp_netif_sntp_deinit`. NVS, the event loop, and the LwIP stack are started once per boot because they cannot be torn down
  (`esp_netif_deinit` is not supported by this ESP-IDF; see the source `components/esp_netif/lwip/esp_netif_lwip.c`).
- `platform/esp-idf/palanu/link_ble.c`: `pal_link_ble_start()` calls `nimble_port_init` (controller and host),
  `ble_gatts_reset`, then builds the GATT and the host task. `pal_link_ble_stop()` disconnects the client, `ble_gap_adv_stop`,
  `nimble_port_stop`, and `nimble_port_deinit`.
- The saved networks (up to 8, `pal/wifi.h`) are one blob the system writes and the board keeps in NVS (`wifi/nets`), out of reach of
  the shell and Palanu Link since it holds the passwords; the older single `ssid`/`pass` pair is moved into it once. The boot asks
  `has_credentials` without the driver to know whether it needs to start Wi-Fi. At start the station joins the most recent network
  with Auto-Join on; after five quick retries it goes on to the next saved one, with the growing delay between rounds. A network
  with a manual address gets it on `WIFI_EVENT_STA_CONNECTED` (DHCP stopped, `esp_netif_set_ip_info`, the DNS set).
- Radio ops and the address hook are registered through `pal_boot_cfg_t.platform_services` (`pal_esp_register_services`, `svc_esp.c`).
  The radio is started by the supervisor according to settings, not directly from `main`.

## Simulator

`platform/host/sim_main.c` registers fake radio operations that only change state. Policy, admission, and the
commands behave the same as on the board. Wi-Fi has no driver in the simulator, so `wifi` reports unavailable.

## Tests

- `tests/unit/test_radio.c`: a single start for many holders, holder limit, grace period, admission (free memory
  and block), a failed start then retry, force off, bad arguments, and a thread test.
- Board: `./pal hwtest --scenario idle|wifi|ble|both|full` adds the `radio` output to the raw section.

## How to measure

1. Boot without radio: `./pal hwtest --scenario idle`. Record free internal RAM and the largest block.
2. Turn on Wi-Fi with `wifi on` then run `free`. The difference in free internal RAM before and after turning it on
   is the Wi-Fi cost. Repeat for `ble on`.
3. Replace `RADIO_COST_WIFI_KB` and `RADIO_COST_BLE_KB` in `system/src/radio.c` with the measured results, run
   `./pal test`, then record the numbers in `docs/SIZE-BUDGETS.md` through `./pal hwtest`.
4. Coexistence: `wifi on`, `ble on`, then `./pal hwtest --scenario both`. Target: free internal RAM >= 86 KB and
   largest block >= 32 KB.

## Not yet tested on the board

- The sequence `esp_wifi_stop`, `esp_wifi_deinit`, then `esp_wifi_init` again within one boot, and whether RAM really
  returns to the heap.
- Starting BLE after `nimble_port_deinit`. This path exists in the IDF source (`nimble_port.c`, `ble_npl_reset_deinit_flag`), but
  has never been run on a device.
- Whether `ble_gatts_reset` clears the service table correctly before the GATT is rebuilt.
