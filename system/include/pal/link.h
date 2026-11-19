/* Palanu Link: the remote protocol, device side. Spec: docs/PLP.md.
 *
 * A link instance serves one connection over any byte stream. The transport feeds received bytes in with
 * pal_link_feed(); the link calls back to transmit. Bytes outside frames are returned to the caller so a UART
 * can keep serving a plain console on the same wire.
 *
 * Trust (docs/PLP.md, "Trust"): a cable link (UART) is trusted unless link.uart.trust is off. A TCP or BLE link is
 * untrusted until a client authenticates on it with a token from pairing. An untrusted link answers only HELLO, PING,
 * and the pairing and auth messages; every other channel is refused with NEMA_ERR_AUTH. */
#ifndef PAL_LINK_H
#define PAL_LINK_H

#include <stdbool.h>

#include "nema/nema.h"

typedef struct pal_link pal_link_t;

typedef enum {
    PAL_LINK_T_NONE = 0, /* not a transport: a link without one is never trusted */
    PAL_LINK_UART = 1,   /* the cable: trusted by default (link.uart.trust) */
    PAL_LINK_TCP = 2,    /* Wi-Fi: untrusted until AUTH */
    PAL_LINK_BLE = 3,    /* Bluetooth: untrusted until AUTH */
    PAL_LINK_WS = 4,     /* WebSocket on Wi-Fi or LAN (Web Forge in a browser): untrusted until AUTH */
} pal_link_transport_t;

typedef struct {
    /* Transmit n bytes. Blocking is fine; called with the link's transmit lock held. Return <0 on failure. */
    int (*tx)(void *ctx, const uint8_t *data, size_t n);
    /* Bytes that were not part of a frame (UART console input). May be NULL. */
    void (*passthrough)(void *ctx, const uint8_t *data, size_t n);
    void *ctx;
    pal_link_transport_t transport; /* sets the trust rule; left unset, the link is untrusted */
} pal_link_io_t;

pal_link_t *pal_link_create(const pal_link_io_t *io);
void pal_link_destroy(pal_link_t *l);
void pal_link_feed(pal_link_t *l, const uint8_t *data, size_t n);
/* A client is connected and authenticated (trusted) and has said hello. Drives the status bar. */
bool pal_link_connected(void);

/* Serve PLP on a TCP port (one client at a time); also usable on the board once Wi-Fi is up. */
nema_err_t pal_link_serve_tcp(uint16_t port);
/* Stop serving: the listener closes and a connected client is dropped. The server task exits within about a second.
 * Call serve and stop from one task (the radio hook does). */
void pal_link_stop_tcp(void);
/* Serve PLP over WebSocket on a port (link_ws.c): what a browser can reach on Wi-Fi or LAN. Same rules as TCP. */
nema_err_t pal_link_serve_ws(uint16_t port);
void pal_link_stop_ws(void);
/* The simulator's --link-open: TCP and WebSocket links are trusted from the start. Tests only; the default is untrusted. */
void pal_link_set_tcp_trust(bool open);

/* ---- pairing and trust (docs/PLP.md) ---- */
#define PAL_LINK_CODE_LEN 6
#define PAL_LINK_E_PAIR_CLOSED (-27) /* no pairing window is open, or it closed (time out, used, or 3 wrong codes) */
/* Opens a pairing window of 120 s with a new 6-digit code (written to code, NUL-terminated). Replaces an open window. */
nema_err_t pal_link_pair_open(char code[PAL_LINK_CODE_LEN + 1]);
void pal_link_pair_close(void);
/* One line about the window: open with the seconds left and the tries left, or closed. Returns the length. */
size_t pal_link_pair_status(char *out, size_t cap);
/* One line per paired client: id, name, created, last used. Returns the length. */
size_t pal_link_clients_text(char *out, size_t cap);
/* How many devices are paired: the record names only, nothing opened or decrypted (cheap enough for a screen). */
int pal_link_clients_count(void);
/* Removes a paired client (16 hex digits of its id, or "all") and drops its connected session at once. */
nema_err_t pal_link_revoke(const char *id);
/* link.uart.trust: sets the value (kept in /data/link/config) and applies it to the live cable link. */
nema_err_t pal_link_uart_trust(bool on);
bool pal_link_uart_trusted(void);
/* The device's random 16-hex identifier (kept in /data/link/device_id). Not a secret: clients use it to find their token. */
void pal_link_device_id(char out[17]);

#endif
