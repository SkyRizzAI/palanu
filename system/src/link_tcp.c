/* Palanu Link over TCP: port 7000 on the board once Wi-Fi has an address, and `palanu-sim --link PORT`. One connection at
 * a time. Each accepted connection is a link with the TCP transport: untrusted until it authenticates (docs/PLP.md,
 * "Trust"), unless the simulator was started with --link-open (tests). */
#include "link_int.h"
#include "nema/nema.h"
#include "nema/port.h"

/* One client at a time: a connection that has not authenticated in this long is closed, so an unknown peer cannot hold the
 * only slot (docs/PLP.md, "Model ancaman"). */
#define UNTRUSTED_IDLE_US 60000000ull

/* The listener runs while the network has an address: the radio hook starts and stops it. Each server task carries the
 * generation it was started in (low 8 bits, packed with the port in its argument). Stopping bumps the generation, so
 * a task that is still winding down exits without touching a newer server. */
static volatile uint32_t s_tcp_gen;
static volatile bool s_tcp_on;
static volatile bool s_tcp_open;

void pal_link_set_tcp_trust(bool open) { s_tcp_open = open; }
bool link_tcp_open(void) { return s_tcp_open; }

static int tcp_tx(void *ctx, const uint8_t *d, size_t n) {
    return nema_obj_write(ctx, d, n, 5000) == (int)n ? 0 : -1;
}

static bool tcp_alive(uint32_t gen8) { return s_tcp_on && (s_tcp_gen & 0xffu) == gen8; }

static void tcp_server(void *arg) {
    uintptr_t v = (uintptr_t)arg;
    uint16_t port = (uint16_t)(v & 0xffffu);
    uint32_t gen8 = (uint32_t)(v >> 16) & 0xffu;
    nema_obj_t *ln = NULL;
    while (tcp_alive(gen8) && nema_net_listen(port, &ln) != NEMA_OK) nema_sleep_ms(2000);
    while (tcp_alive(gen8)) {
        nema_obj_t *c;
        if (nema_net_accept(ln, 1000, &c) != NEMA_OK) continue;
        if (!tcp_alive(gen8)) {
            nema_obj_unref(c);
            break;
        }
        pal_link_io_t io = {.tx = tcp_tx, .ctx = c, .transport = PAL_LINK_TCP};
        pal_link_t *l = pal_link_create(&io);
        if (l) {
            uint8_t b[256];
            uint64_t t0 = nema_port_time_us();
            /* a revoked client is dropped here: its session ends and the socket closes */
            while (tcp_alive(gen8) && !l->kill && (l->trusted || nema_port_time_us() - t0 < UNTRUSTED_IDLE_US)) {
                int n = nema_obj_read(c, b, sizeof(b), 500);
                if (n == NEMA_ERR_TIMEOUT) continue;
                if (n <= 0) break;
                pal_link_feed(l, b, (size_t)n);
            }
            pal_link_destroy(l);
        }
        nema_obj_unref(c);
    }
    if (ln) nema_obj_unref(ln);
}

nema_err_t pal_link_serve_tcp(uint16_t port) {
    if (s_tcp_on) return NEMA_OK;
    s_tcp_gen++;
    s_tcp_on = true;
    uintptr_t arg = ((uintptr_t)(s_tcp_gen & 0xffu) << 16) | port;
    nema_task_desc_t td = {.name = "link-tcp", .fn = tcp_server, .arg = (void *)arg, .stack = 6144,
                           .prio = 3, .core = -1, .role = NEMA_TASK_SERVICE, .owner = nema_owner_register("link")};
    nema_err_t r = nema_task_create(&td);
    if (r != NEMA_OK) s_tcp_on = false;
    return r;
}

void pal_link_stop_tcp(void) {
    s_tcp_on = false;
    s_tcp_gen++;
}
