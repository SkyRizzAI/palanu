/* ssh: an SSH client on top of libssh, driven through Nema sockets and the TTY.
 *
 * The session is non-blocking. Waiting is one nema_poll() over the socket and the keyboard, so typing,
 * remote output, and window resizes are all handled by the same loop without extra tasks. */
#if __has_include(<libssh/libssh.h>)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libssh/libssh.h>

#include "libapp.h"
#include "pal/term.h"

#define KNOWN_HOSTS "/data/ssh/known_hosts"

typedef struct {
    ssh_session s;
    ssh_channel ch;
    nema_obj_t *sock;
    int sockfd; /* handle in this process, for nema_poll */
} conn_t;

/* Wait until the socket is ready (or a short timeout), then let the caller retry. */
static int wait_io(conn_t *c, int32_t ms) {
    nema_pollfd_t p = {c->sockfd, NEMA_POLL_IN | NEMA_POLL_OUT, 0};
    int r = nema_poll(&p, 1, ms);
    if (r == NEMA_ERR_INTR) {
        nema_sig_take(NEMA_SIG_WINCH);
        return nema_interrupted() ? -1 : 0;
    }
    return r < 0 ? -1 : 0;
}

static void say(const char *fmt, const char *a, const char *b) { pal_eprintf(fmt, a, b); }

static int read_password(char *out, size_t cap);

#define KEY_DIR "/home/.ssh"
#define MAX_KEYS 4

/* Read a whole file into a NUL-terminated string (nema_free it), or NULL. */
static char *read_text(const char *path) {
    int fd = nema_open(path, NEMA_O_READ);
    if (fd < 0) return NULL;
    size_t n;
    char *t = pal_read_all(fd, &n);
    nema_close(fd);
    if (!t) return NULL;
    char *z = nema_alloc(NEMA_MEM_ANY, nema_self() ? nema_self()->owner : NEMA_OWNER_KERNEL, n + 1);
    if (z) {
        memcpy(z, t, n);
        z[n] = 0;
    }
    nema_free(t);
    return z;
}

/* Load a private key; an encrypted key asks for its passphrase once. */
static ssh_key load_key(const char *path) {
    char *text = read_text(path);
    if (!text) return NULL;
    ssh_key k = NULL;
    if (ssh_pki_import_privkey_base64(text, NULL, NULL, NULL, &k) != SSH_OK) {
        char pw[64];
        k = NULL;
        pal_eprintf("Enter passphrase for key '%s': ", path);
        if (read_password(pw, sizeof(pw)) == 0) {
            if (ssh_pki_import_privkey_base64(text, pw, NULL, NULL, &k) != SSH_OK) k = NULL;
            memset(pw, 0, sizeof(pw));
        }
    }
    nema_free(text);
    return k;
}

static int write_text(const char *path, const char *text) {
    int fd = nema_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC);
    if (fd < 0) return fd;
    int w = nema_write(fd, text, strlen(text));
    nema_close(fd);
    return w < 0 ? w : 0;
}

/* ssh-keygen [-f path]: make an ed25519 key pair; the public half is printed to paste into authorized_keys. */
static int cmd_keygen(int argc, char **argv) {
    const char *path = KEY_DIR "/id_ed25519";
    enum ssh_keytypes_e type = SSH_KEYTYPE_ED25519;
    const char *tname = "ssh-ed25519";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) path = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            if (strcmp(t, "ecdsa") == 0) type = SSH_KEYTYPE_ECDSA_P256, tname = "ecdsa-sha2-nistp256", path = KEY_DIR "/id_ecdsa";
            else if (strcmp(t, "rsa") == 0) type = SSH_KEYTYPE_RSA, tname = "ssh-rsa", path = KEY_DIR "/id_rsa";
        } else if (strcmp(argv[i], "-v") == 0) {
            ssh_set_log_level(SSH_LOG_DEBUG);
        }
    }
    nema_fs_mkdir(KEY_DIR);
    nema_stat_t st;
    if (nema_fs_stat(path, &st) == NEMA_OK) {
        pal_eprintf("ssh-keygen: %s already exists; remove it first or choose another with -f\n", path);
        return 1;
    }
    pal_eprintf("generating a %s key...\n", tname);
    ssh_init(); /* sets up the random number generator the key code needs */
    ssh_key key = NULL;
    if (ssh_pki_generate_key(type, NULL, &key) != SSH_OK) {
        pal_eprintf("ssh-keygen: key generation failed\n");
        return 1;
    }
    char *priv = NULL, *pub = NULL;
    int rc = 1;
    if (ssh_pki_export_privkey_base64(key, NULL, NULL, NULL, &priv) == SSH_OK &&
        ssh_pki_export_pubkey_base64(key, &pub) == SSH_OK) {
        char pubpath[80], line[1024];
        snprintf(pubpath, sizeof(pubpath), "%s.pub", path);
        snprintf(line, sizeof(line), "%s %s palanu\n", tname, pub);
        if (write_text(path, priv) == 0 && write_text(pubpath, line) == 0) {
            pal_printf("saved %s and %s\nadd this line to ~/.ssh/authorized_keys on the server:\n\n%s", path, pubpath, line);
            rc = 0;
        } else {
            pal_eprintf("ssh-keygen: cannot write the key files\n");
        }
    }
    ssh_string_free_char(priv);
    ssh_string_free_char(pub);
    ssh_key_free(key);
    return rc;
}

static int hostkey_check(ssh_session s, const char *host, int port) {
    ssh_key key = NULL;
    unsigned char *hash = NULL;
    size_t hlen = 0;
    if (ssh_get_server_publickey(s, &key) != SSH_OK) return -1;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != SSH_OK) {
        ssh_key_free(key);
        return -1;
    }
    char *fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
    char id[128];
    snprintf(id, sizeof(id), "%s:%d", host, port);
    const char *type = ssh_key_type_to_char(ssh_key_type(key));

    int result = 0;
    char *known = NULL;
    size_t klen = 0;
    int fd = nema_open(KNOWN_HOSTS, NEMA_O_READ);
    if (fd >= 0) {
        known = pal_read_all(fd, &klen);
        nema_close(fd);
    }
    bool seen = false;
    if (known) {
        char *save = NULL;
        for (char *line = strtok_r(known, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char h[128], t[32], f[96];
            if (sscanf(line, "%127s %31s %95s", h, t, f) != 3 || strcmp(h, id) != 0) continue;
            seen = true;
            if (strcmp(t, type) == 0 && strcmp(f, fp) != 0) {
                pal_eprintf("\x1b[1;31mWARNING: the host key for %s has CHANGED.\x1b[0m\nExpected %s\nGot      %s\n", id, f, fp);
                pal_eprintf("Someone may be intercepting this connection. Remove the line from %s only if you are sure.\n", KNOWN_HOSTS);
                result = -1;
            } else if (strcmp(t, type) == 0) {
                result = 1; /* matches */
            }
        }
        nema_free(known);
    }
    if (result == 0) { /* unknown host: ask */
        pal_eprintf("The authenticity of host '%s' cannot be established.\n%s key fingerprint is %s\nTrust this host and continue (yes/no)? ", id, type, fp);
        char ans[16];
        int n = nema_read(0, ans, sizeof(ans) - 1);
        ans[n > 0 ? n : 0] = 0;
        if (strncmp(ans, "yes", 3) == 0) {
            nema_fs_mkdir("/data/ssh");
            nema_obj_t *f;
            if (nema_fs_open(KNOWN_HOSTS, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_APPEND, &f) == NEMA_OK) {
                char line[256];
                int l = snprintf(line, sizeof(line), "%s %s %s\n", id, type, fp);
                nema_obj_write(f, line, (size_t)l, 0);
                nema_obj_unref(f);
            }
            result = 1;
        } else {
            result = -1;
        }
    }
    (void)seen;
    ssh_string_free_char(fp);
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(key);
    return result > 0 ? 0 : -1;
}

static int read_password(char *out, size_t cap) {
    uint32_t mode = NEMA_TTY_ONLCR | NEMA_TTY_ISIG | NEMA_TTY_ICANON; /* line input without echo */
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    int n = nema_read(0, out, cap - 1);
    mode = NEMA_TTY_COOKED;
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    pal_puts("\n");
    if (n <= 0) return -1;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    out[n] = 0;
    return n;
}

static void tty_size(int *cols, int *rows) {
    nema_winsize_t ws;
    if (nema_ioctl(0, NEMA_TIOC_GETSIZE, &ws) == NEMA_OK) {
        *cols = ws.cols;
        *rows = ws.rows;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

/* The pty modes a normal interactive terminal has (RFC 4254 section 8, opcode then uint32 big-endian): echo, line editing,
 * signals from the keyboard (Ctrl-C interrupt, Ctrl-\\ quit, Ctrl-Z suspend, Ctrl-D end of file), Backspace as DEL (0x7f,
 * what this keyboard sends), CR to NL on input and NL to CR NL on output. */
static size_t pty_modes(unsigned char *out, size_t cap) {
    static const struct {
        unsigned char op;
        uint32_t v;
    } M[] = {
        {1, 0x03},  /* VINTR: Ctrl-C */
        {2, 0x1c},  /* VQUIT: Ctrl-\ */
        {3, 0x7f},  /* VERASE: DEL */
        {4, 0x15},  /* VKILL: Ctrl-U */
        {5, 0x04},  /* VEOF: Ctrl-D */
        {10, 0x1a}, /* VSUSP: Ctrl-Z */
        {36, 1},    /* ICRNL */
        {50, 1},    /* ISIG */
        {51, 1},    /* ICANON */
        {53, 1},    /* ECHO */
        {54, 1},    /* ECHOE */
        {55, 1},    /* ECHOK */
        {59, 1},    /* IEXTEN */
        {60, 1},    /* ECHOCTL */
        {70, 1},    /* OPOST */
        {72, 1},    /* ONLCR */
        {91, 1},    /* CS8 */
        {128, 115200}, /* TTY_OP_ISPEED */
        {129, 115200}, /* TTY_OP_OSPEED */
    };
    size_t n = 0;
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]) && n + 6 <= cap; i++) {
        out[n++] = M[i].op;
        out[n++] = (unsigned char)(M[i].v >> 24);
        out[n++] = (unsigned char)(M[i].v >> 16);
        out[n++] = (unsigned char)(M[i].v >> 8);
        out[n++] = (unsigned char)M[i].v;
    }
    if (n < cap) out[n++] = 0; /* TTY_OP_END */
    return n;
}

static int relay(conn_t *c) {
    char buf[512];
    int cols, rows;
    tty_size(&cols, &rows);
    bool at_line_start = true, tilde = false;
    for (;;) {
        int n;
        bool got = false;
        while ((n = ssh_channel_read_nonblocking(c->ch, buf, sizeof(buf), 0)) > 0) { nema_write(1, buf, (size_t)n); got = true; }
        while ((n = ssh_channel_read_nonblocking(c->ch, buf, sizeof(buf), 1)) > 0) { nema_write(1, buf, (size_t)n); got = true; }
        if (n == SSH_EOF || ssh_channel_is_eof(c->ch) || !ssh_channel_is_open(c->ch)) return 0;
        if (n == SSH_ERROR) return 1;
        if (got) continue;

        nema_pollfd_t p[2] = {{c->sockfd, NEMA_POLL_IN, 0}, {0, NEMA_POLL_IN, 0}};
        int r = nema_poll(p, 2, 200);
        if (r == NEMA_ERR_INTR) {
            if (nema_sig_take(NEMA_SIG_WINCH)) {
                int nc, nr;
                tty_size(&nc, &nr);
                if (nc != cols || nr != rows) {
                    cols = nc, rows = nr;
                    ssh_channel_change_pty_size(c->ch, cols, rows);
                }
            }
            if (nema_interrupted()) return 130;
            continue;
        }
        if (r > 0 && p[1].revents) {
            int k = nema_read(0, buf, sizeof(buf));
            if (k <= 0) return 0;
            for (int i = 0; i < k; i++) { /* "~." at the start of a line disconnects, as in OpenSSH */
                if (tilde && buf[i] == '.') return 0;
                tilde = at_line_start && buf[i] == '~';
                at_line_start = buf[i] == '\r' || buf[i] == '\n';
            }
            int off = 0;
            while (off < k) {
                int w = ssh_channel_write(c->ch, buf + off, (uint32_t)(k - off));
                if (w == SSH_ERROR) return 1;
                if (w == SSH_AGAIN || w == 0) {
                    if (wait_io(c, 100) < 0) return 130;
                    continue;
                }
                off += w;
            }
        }
    }
}

static int cmd_ssh(int argc, char **argv) {
    const char *user = "palanu";
    int port = 22;
    const char *target = NULL;
    const char *keyfiles[MAX_KEYS];
    int nkeys = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            if (nkeys < MAX_KEYS) keyfiles[nkeys++] = argv[++i];
            else i++;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) user = argv[++i];
        else if (argv[i][0] != '-') target = argv[i];
    }
    if (!target) {
        pal_eprintf("usage: ssh [-p port] [-l user] [-i keyfile] [user@]host\n");
        return 255;
    }
    char hostbuf[96];
    nema_strlcpy(hostbuf, target, sizeof(hostbuf));
    char *at = strchr(hostbuf, '@');
    const char *host = hostbuf;
    char userbuf[40];
    if (at) {
        *at = 0;
        nema_strlcpy(userbuf, hostbuf, sizeof(userbuf));
        user = userbuf;
        host = at + 1;
    }

    conn_t c = {0};
    pal_eprintf("connecting to %s:%d...\n", host, port);
    nema_err_t e = nema_net_connect(host, (uint16_t)port, 15000, &c.sock);
    if (e != NEMA_OK) {
        pal_eprintf("ssh: %s: %s\n", host, pal_strerror(e));
        return 255;
    }
    c.sockfd = nema_handle_install(c.sock);
    int osfd = -1;
    nema_obj_t *so = c.sock;
    so->ops->ioctl(so, NEMA_NETIOC_GETFD, &osfd);
    nema_obj_unref(c.sock);

    /* libssh expands "%d" (the default keys "%d/.ssh/id_ed25519") to the user's home: getpwuid_r, then $HOME. The board's
     * libssh port makes getpwuid_r fail and the board has no $HOME, so every connect failed with "Couldn't apply options"
     * (found on the board; the host has a home, so the end-to-end test passed). /home is where KEY_DIR lives. An existing
     * HOME (the host) is kept. */
    setenv("HOME", "/home", 0);
    ssh_init();
    c.s = ssh_new();
    int rc = 255;
    if (!c.s) goto out_sock;
    long timeout = 30;
    int off = 0;
    ssh_options_set(c.s, SSH_OPTIONS_HOST, host);
    ssh_options_set(c.s, SSH_OPTIONS_PORT, &port);
    ssh_options_set(c.s, SSH_OPTIONS_USER, user);
    ssh_options_set(c.s, SSH_OPTIONS_FD, &osfd);
    ssh_options_set(c.s, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(c.s, SSH_OPTIONS_PROCESS_CONFIG, &off);
    /* Paths given outright, so they never depend on the home lookup above. */
    ssh_options_set(c.s, SSH_OPTIONS_SSH_DIR, KEY_DIR);
    ssh_options_set(c.s, SSH_OPTIONS_KNOWNHOSTS, KNOWN_HOSTS);
    ssh_options_set(c.s, SSH_OPTIONS_STRICTHOSTKEYCHECK, &off);
    ssh_set_blocking(c.s, 0);

    int r;
    while ((r = ssh_connect(c.s)) == SSH_AGAIN)
        if (wait_io(&c, 100) < 0) goto out_ssh;
    if (r != SSH_OK) {
        pal_eprintf("ssh: connection failed: %s\n", ssh_get_error(c.s));
        goto out_ssh;
    }
    if (hostkey_check(c.s, host, port) != 0) {
        pal_eprintf("ssh: host key verification failed.\n");
        goto out_ssh;
    }

    int auth = SSH_AUTH_DENIED;
    nema_stat_t kst;
    if (nkeys == 0 && nema_fs_stat(KEY_DIR "/id_ed25519", &kst) == NEMA_OK) keyfiles[nkeys++] = KEY_DIR "/id_ed25519";
    if (nkeys == 0 && nema_fs_stat(KEY_DIR "/id_rsa", &kst) == NEMA_OK) keyfiles[nkeys++] = KEY_DIR "/id_rsa";
    for (int k = 0; k < nkeys && auth != SSH_AUTH_SUCCESS; k++) {
        ssh_key key = load_key(keyfiles[k]);
        if (!key) {
            pal_eprintf("ssh: cannot use key %s\n", keyfiles[k]);
            continue;
        }
        while ((auth = ssh_userauth_publickey(c.s, NULL, key)) == SSH_AUTH_AGAIN)
            if (wait_io(&c, 100) < 0) {
                ssh_key_free(key);
                goto out_ssh;
            }
        ssh_key_free(key);
    }
    char pw[64];
    for (int attempt = 0; attempt < 3 && auth != SSH_AUTH_SUCCESS; attempt++) {
        pal_eprintf("%s@%s's password: ", user, host);
        if (read_password(pw, sizeof(pw)) < 0) goto out_ssh;
        while ((auth = ssh_userauth_password(c.s, NULL, pw)) == SSH_AUTH_AGAIN)
            if (wait_io(&c, 100) < 0) goto out_ssh;
        memset(pw, 0, sizeof(pw));
        if (auth != SSH_AUTH_SUCCESS) pal_eprintf("Permission denied, please try again.\n");
    }
    if (auth != SSH_AUTH_SUCCESS) {
        pal_eprintf("ssh: too many authentication failures\n");
        goto out_ssh;
    }

    c.ch = ssh_channel_new(c.s);
    if (!c.ch) goto out_ssh;
    while ((r = ssh_channel_open_session(c.ch)) == SSH_AGAIN)
        if (wait_io(&c, 100) < 0) goto out_chan;
    if (r != SSH_OK) {
        pal_eprintf("ssh: cannot open a session: %s\n", ssh_get_error(c.s));
        goto out_chan;
    }
    int cols, rows;
    tty_size(&cols, &rows);
    /* The terminal modes are sent outright. ssh_channel_request_pty_size reads them from the local terminal with
     * tcgetattr(0), which on the board is ESP-IDF's console UART, not this TTY: the server's pty then had no echo, no
     * onlcr (each prompt began where the last line ended) and no Ctrl-C (found on the board). */
    unsigned char modes[128]; /* 19 modes of 5 bytes and the end mark */
    size_t mlen = pty_modes(modes, sizeof(modes));
    while ((r = ssh_channel_request_pty_size_modes(c.ch, "xterm-256color", cols, rows, modes, mlen)) == SSH_AGAIN)
        if (wait_io(&c, 100) < 0) goto out_chan;
    if (r == SSH_OK) {
        /* The locale, as OpenSSH's SendEnv does: without it the server's programs run in the C locale, and output such as
         * a progress bar of "■" comes out as "?" with the wrong width (found with an installer on the board). The terminal
         * decodes UTF-8. A server that does not accept LANG (no AcceptEnv) refuses it; that is not an error. */
        while (ssh_channel_request_env(c.ch, "LANG", "C.UTF-8") == SSH_AGAIN)
            if (wait_io(&c, 100) < 0) goto out_chan;
    }
    if (r == SSH_OK) /* the pty request was accepted: now the shell. AGAIN means the reply is not here yet */
        while ((r = ssh_channel_request_shell(c.ch)) == SSH_AGAIN)
            if (wait_io(&c, 100) < 0) goto out_chan;
    if (r != SSH_OK) {
        pal_eprintf("ssh: cannot start a shell: %s\n", ssh_get_error(c.s));
        goto out_chan;
    }

    pal_eprintf("(Ctrl-D or exit ends the session; ~. at the start of a line disconnects at once)\r\n");
    uint32_t mode = NEMA_TTY_RAW; /* the remote side does the editing and the echo */
    nema_ioctl(0, NEMA_TIOC_SETMODE, &mode);
    rc = relay(&c);
    pal_puts("\x1b[0m\x1b[?25h\x1b[?1049l\r\nConnection closed.\r\n");

out_chan:
    ssh_channel_close(c.ch);
    ssh_channel_free(c.ch);
out_ssh:
    ssh_disconnect(c.s);
    ssh_free(c.s);
out_sock:
    nema_close(c.sockfd);
    (void)say;
    return rc;
}

static const nema_app_t SSH_APP = {"ssh", "SSH client: ssh [-p port] [-i key] [user@]host", cmd_ssh, 20 * 1024, 0};
static const nema_app_t KEYGEN_APP = {"ssh-keygen", "make an SSH key pair", cmd_keygen, 20 * 1024, 0};

void pal_apps_register_ssh(void);
void pal_apps_register_ssh(void) {
    nema_app_register(&SSH_APP);
    nema_app_register(&KEYGEN_APP);
}

#else /* no libssh on this build (the simulator) */

#include "libapp.h"

void pal_apps_register_ssh(void);
void pal_apps_register_ssh(void) {}

#endif
