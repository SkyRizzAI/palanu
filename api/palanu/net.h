/* TCP and DNS, as a client or a server (IPv4). A socket is an ordinary handle: pal_read, pal_write, pal_close, and pal_wait
 * work on it. Connect, resolve, and accept take a timeout and stop at once when the app is asked to end (Ctrl-C, kill).
 * UDP is not in this level. */
#ifndef PALANU_API_NET_H
#define PALANU_API_NET_H

#include "annot.h"

/* Error codes: negative values are errors (same numbers as the system). */
#define PAL_ERR_TIMEOUT (-5)
#define PAL_ERR_REFUSED (-26)
#define PAL_ERR_RESET (-24)
#define PAL_ERR_UNREACH (-25)

/* Layout of the pal_net_info record (12 bytes): uint8 up, 3 bytes padding, uint8 ip[4] (a, b, c, d; 0.0.0.0 when down),
 * int32 rssi in dBm (0 when not Wi-Fi). Little-endian for the integers. */
#define PAL_NET_INFO_SIZE 12

PAL_SINCE(2) PAL_CAP(net) PAL_WASM(tcp_connect)
PAL_DOC("Connects to host (a name or a dotted IPv4 address, NUL-terminated) on port, waiting up to timeout_ms (negative: no limit) for the lookup and the connect. Returns a handle, or a negative error: PAL_ERR_REFUSED, PAL_ERR_UNREACH, PAL_ERR_TIMEOUT, PAL_ERR_NOENT.")
int32_t pal_tcp_connect(const char *host, int32_t port, int32_t timeout_ms);

PAL_SINCE(2) PAL_CAP("net.listen") PAL_WASM(tcp_listen)
PAL_DOC("Listens on TCP port (all addresses) with the given backlog (1 to 32). Returns a listener handle, or a negative error.")
int32_t pal_tcp_listen(int32_t port, int32_t backlog);

PAL_SINCE(2) PAL_CAP("net.listen") PAL_WASM(tcp_accept)
PAL_DOC("Waits up to timeout_ms (negative: no limit) for a connection on listener and returns a handle to it, or a negative error (PAL_ERR_TIMEOUT when none came).")
int32_t pal_tcp_accept(int32_t listener, int32_t timeout_ms);

PAL_SINCE(2) PAL_CAP(net) PAL_WASM(dns_resolve)
PAL_DOC("Resolves host to an IPv4 address and writes its 4 bytes (a, b, c, d) into addr_out, which needs len >= 4. Waits up to timeout_ms. Returns 0, or a negative error.")
int32_t pal_dns_resolve(const char *host, PAL_OUT(len) void *addr_out, uint32_t len, int32_t timeout_ms);

PAL_SINCE(2) PAL_CAP(net) PAL_WASM(net_info)
PAL_DOC("Writes the network state into out (12 bytes, see PAL_NET_INFO_SIZE): whether the link is up, its IPv4 address and the Wi-Fi signal. Returns 0, or a negative error.")
int32_t pal_net_info(PAL_OUT(len) void *out, uint32_t len);

PAL_SINCE(2) PAL_WASM(set_nonblock)
PAL_DOC("on != 0: reads and writes on handle h never wait; with no data or no room they return PAL_ERR_AGAIN. on == 0: they wait again. Works on any handle the app owns.")
int32_t pal_set_nonblock(int32_t h, int32_t on);

PAL_SINCE(2) PAL_WASM(shutdown)
PAL_DOC("Shuts down a socket the app owns: how is 0 (receiving), 1 (sending) or 2 (both). Returns 0, or a negative error.")
int32_t pal_shutdown(int32_t h, int32_t how);

#endif
