/* Nema networking: TCP sockets as ordinary objects.
 *
 * A socket is a nema_obj_t, so it works with read, write, poll, and dup like a pipe or a TTY. That is what lets
 * an SSH client wait on the network and the keyboard at once with one nema_poll(). The implementation sits on
 * the BSD socket API that both lwIP (board) and the host provide.
 *
 * Name lookup and connect wait in nema_wait_on(), so a fatal signal (TERM, KILL) ends them at once; the lookup itself
 * runs in a short helper task, because getaddrinfo() cannot be interrupted.
 */
#ifndef NEMA_NET_H
#define NEMA_NET_H

#include "nema/obj.h"

/* ioctl on a socket object: *(int *)arg receives the OS file descriptor, for libraries that insist on one. */
#define NEMA_NETIOC_GETFD 0x6E01u
/* ioctl on a socket object: shuts down one or both directions. arg: int *how (0 read, 1 write, 2 both). */
#define NEMA_NETIOC_SHUTDOWN 0x6E02u

void nema_net_init(void); /* starts the socket pump task; call once after nema_init */

/* Pump wake-ups since boot (each return of the pump from its wait). Idle sockets cost none. For tests and diagnostics. */
uint32_t nema_net_pump_wakeups(void);

/* Resolve host (a name or a dotted IPv4 address) to four address bytes, in network order (a, b, c, d). Waits up to
 * timeout_ms (< 0: no limit). NEMA_ERR_NOENT if the name is unknown, NEMA_ERR_TIMEOUT if the lookup is still running. */
nema_err_t nema_net_resolve_addr(const char *host, int32_t timeout_ms, uint8_t ip[4]);
/* Resolve host to dotted IPv4 text. Waits as long as the lookup takes (see nema_net_resolve_addr for the errors). */
nema_err_t nema_net_resolve(const char *host, char *ip, size_t cap);

/* Connect to host:port. The name lookup and the connect share timeout_ms (< 0: no limit). Refused, unreachable, and
 * reset connections get their own codes (NEMA_ERR_REFUSED, NEMA_ERR_UNREACH, NEMA_ERR_RESET). A fatal signal
 * interrupts both steps. */
nema_err_t nema_net_connect(const char *host, uint16_t port, int32_t timeout_ms, nema_obj_t **out);

/* Listen on a TCP port with the given backlog; reading from the returned object is not supported: use nema_net_accept(). */
nema_err_t nema_net_listen_bl(uint16_t port, int backlog, nema_obj_t **out);
nema_err_t nema_net_listen(uint16_t port, nema_obj_t **out); /* backlog 2 */
/* Waits up to timeout_ms (< 0: no limit) for a connection. NEMA_ERR_INVAL if listener is not a listening socket. */
nema_err_t nema_net_accept(nema_obj_t *listener, int32_t timeout_ms, nema_obj_t **client);

/* Turn a connected socket into a TLS client: the handshake runs now (up to timeout_ms, < 0 no limit; a fatal signal ends it),
 * then reads and writes on the same object carry plain text and the wire is encrypted. host is the server name sent and
 * checked against its certificate; verify false accepts any certificate (curl -k). NEMA_ERR_AUTH: the certificate is not
 * trusted; NEMA_ERR_PROTO: the handshake failed; NEMA_ERR_UNSUPPORTED: this build has no TLS (the browser simulator);
 * NEMA_ERR_STATE: already TLS. On an error the socket is left as it was (close it). */
nema_err_t nema_net_tls(nema_obj_t *sock, const char *host, bool verify, int32_t timeout_ms);
/* One ICMP echo (ping) to host: waits up to timeout_ms for the reply to sequence number seq and gives its round trip in
 * microseconds and the address that answered (4 bytes, a, b, c, d; may be NULL). NEMA_ERR_TIMEOUT when no reply came,
 * NEMA_ERR_NOENT for an unknown name, NEMA_ERR_UNSUPPORTED where the system has no ICMP socket. A fatal signal ends it. */
nema_err_t nema_net_ping(const char *host, uint16_t seq, int32_t timeout_ms, uint32_t *rtt_us, uint8_t ip_out[4]);

/* The TLS library behind it: "mbedtls" (board), "openssl" (native simulator) or "none". */
const char *nema_net_tls_backend(void);

#endif
