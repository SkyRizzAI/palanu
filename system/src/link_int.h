/* Palanu Link internals shared by link.c (framing and channels), link_admin.c (the registry, pairing and trust
 * commands) and link_tcp.c (the TCP transport). Not for the rest of the system: the public API is pal/link.h. */
#ifndef SYSTEM_LINK_INT_H
#define SYSTEM_LINK_INT_H

#include <stdbool.h>
#include <stdint.h>

#include "link_auth.h"
#include "nema/nema.h"
#include "pal/display.h"
#include "pal/link.h"

#define LINK_MAX_LIVE 8 /* links alive at once (cable, TCP, BLE, and a spare) */

struct pal_link {
    pal_link_io_t io;
    nema_owner_t owner;
    nema_port_mutex_t *txlock;
    uint8_t *txbuf; /* 5 + MAX_PAYLOAD + 2 */

    /* decoder */
    uint8_t rx_state;
    uint8_t chan, type;
    uint16_t len, idx;
    uint8_t head[4];
    uint8_t crc_lo;
    uint8_t *rx; /* MAX_PAYLOAD */

    /* remote shell */
    nema_tty_t *tty;
    nema_pid_t pid;

    /* screen mirror */
    bool screen_on;
    bool screen_rgb; /* the client asked for RGB565 rectangles (SUBSCRIBE 2): the screen frame as it is */
    nema_port_mutex_t *dlock;
    nema_port_mutex_t *tlock; /* the shell session: the worker reads it while the receive path opens and closes it */
    pal_rect_t dirty; /* empty when w == 0 */
    bool send_palette;

    /* file upload in progress */
    nema_obj_t *wfile;
    char wpath[64]; /* where the upload goes; a package written to /apps becomes a command */

    bool ota_open; /* a firmware image is being received */
    volatile bool dying, worker_done;

    /* trust (docs/PLP.md, "Trust"). trusted: the frames of every channel are accepted. kill: the session ends (a revoked
     * client, or cable trust turned off); set from another task, so read it volatile. */
    pal_link_transport_t tr;
    volatile bool trusted;
    volatile bool kill;
    bool hello; /* a client has said HELLO on this link */
    la_sess_t auth;
};

/* link.c: ends the session's screen, shell, upload and firmware image. Safe to call at any time. */
void link_session_end(pal_link_t *l);

/* link_admin.c: the live links, so a revoke or a trust change reaches them. */
nema_err_t link_reg_add(pal_link_t *l);
void link_reg_del(pal_link_t *l);
bool link_default_trust(pal_link_transport_t tr); /* what a new link of this transport starts as */

/* link_tcp.c: the simulator's --link-open. */
bool link_tcp_open(void);

#endif
