/* The SSH connection the ssh, scp and sftp commands share (apps/cmd_ssh.c): connect over a Nema socket, check the host key against
 * /data/ssh/known_hosts, authenticate with keys and then a password. Only built where libssh is (the board, a host built with it). */
#ifndef PAL_SSH_SESSION_H
#define PAL_SSH_SESSION_H

#include <stdbool.h>

#include <libssh/libssh.h>

#include "libapp.h"

#define SSH_MAX_KEYS 4

typedef struct {
    ssh_session s;
    ssh_channel ch;
    nema_obj_t *sock;
    int sockfd; /* handle in this process, for nema_poll; -1 when closed */
} conn_t;

/* target: [user@]host (or a Host of /home/.ssh/config). keyfiles has room for SSH_MAX_KEYS. who prefixes the messages ("scp").
 * 0 on success, the session authenticated and non-blocking; else an exit status, with everything closed. */
int ssh_open(conn_t *c, const char *who, const char *target, int port, bool port_set, const char *user, bool user_set,
             const char **keyfiles, int nkeys);
void ssh_close(conn_t *c);

#endif
