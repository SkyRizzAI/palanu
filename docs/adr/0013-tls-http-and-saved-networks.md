# ADR-0013: TLS is a property of a socket; one HTTP client; saved networks are the system's list

Status: accepted (19 November 2026).

## Context
The gap analysis of 19 November 2026 found the network layer behind Kairo and every competitor: `curl` did HTTP/1.0 GET only, there
was no TLS, apps had TCP and DNS but no HTTP, one Wi-Fi network could be saved, and there was no manual address and no ping. An app
catalogue, firmware downloads and most daily-use features need HTTPS first.

## Decision
- **TLS is a property of a socket.** `nema_net_tls(sock, host, verify, timeout)` turns a connected Nema socket into a TLS client; reads,
  writes, poll and the pump keep working on the same object, so every user of sockets (curl, the HTTP client, apps through
  `pal_tls_start`, later SSH helpers) gets TLS without a second object type that the pump could not wake. The backend sits beside the
  socket code in `platform/net_bsd/net_tls.c`: mbedtls 4 with the IDF certificate bundle on the board (client only, the common roots,
  its buffers in PSRAM through the IDF malloc), OpenSSL in the native simulator when installed, none in the browser.
- **One HTTP client** in the system (`pal/http.h`): HTTP/1.1, any method, headers, a body, redirects, chunked and Content-Length
  bodies, the body handed over in pieces. `curl` (`-i -I -X -H -d -L -o -k -s`) and the app call `pal_http_fetch` (API level 7) use
  it. An app's TLS always checks the certificate.
- **Saved networks are the system's list** (`pal/wifi.h`, up to 8, the most recent first, Auto-Join and a manual IPv4 address each).
  The list is one blob the board stores where the shell and Palanu Link cannot read it (NVS), because it holds the passwords; the
  board only joins in order and applies a manual address. Settings > Wi-Fi > My networks and `wifi saved|forget|auto|ip` edit it.
- **ping and tcping**: `nema_net_ping` sends one ICMP echo (a datagram ICMP socket on the host, raw on lwIP).

## Consequences
- The image grew by the TLS code and the certificate bundle; the fallback font was compressed in the same change (ADR-less, see
  docs/FONTS.md) and LVGL is built for size, so the image went from 92.7% to 81.7% of the OTA slot.
- TLS and the saved networks were proven in the native simulator and by tests (`test_http`, `test_wifi_nets`, `test_net_api`); the
  board build compiles and links the mbedtls path, which has not run on hhv2 yet.
- The browser simulator has no TLS: `curl https://` says so.
