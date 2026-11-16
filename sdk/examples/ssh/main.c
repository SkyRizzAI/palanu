/* ssh: an SSH client for PalanuOS, written with the public API only (sdk/palanu.h, sdk/palanu_libc.h) and the palssh
 * library (sdk/palssh). Usage:
 *   ssh [-p port] [-i key] [-l user] [-t] [user@]host [command]
 *   ssh keygen [name]         make an ed25519 key in the keystore (default id_ed25519)
 *   ssh pub [name]            print the authorized_keys line of a key
 *   ssh keys                  list the keys in the keystore
 *   ssh import name FILE      import an unencrypted OpenSSH ed25519 key file (FILE in /data or /tmp)
 * Known hosts live in /data/known_hosts (OpenSSH format). A changed host key is refused; there is no override.
 * Build: bun forge/tools/palanu.ts build   (in this directory; writes ssh.papp) */
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"
#define PALSSH_IMPLEMENTATION
#define PALSSH_WITH_PAL
#include "palssh/palssh.h"

#define KNOWN_HOSTS "/data/known_hosts"
#define KNOWN_MAX 16384
#define DEFAULT_KEY "id_ed25519"

static pssh_t S; /* the session state: about 80 KB of buffers, so it is static and not on the 24 KB stack */
static pssh_pal_t P;
static char known[KNOWN_MAX];

static void say(const char *s) { pal_write(1, s, (uint32_t)strlen(s)); }
static void say_err(const char *s) { pal_write(2, s, (uint32_t)strlen(s)); }

/* Copies argument i into buf (NUL-terminated). Returns 0 when it is missing or too long. */
static int word(int32_t i, char *buf, size_t cap) {
    int32_t n = pal_argv(i, buf, (uint32_t)cap);
    return n >= 0 && (size_t)n < cap;
}

/* Appends s to the string in dst (cap bytes); silently stops when it does not fit. */
static void append(char *dst, size_t cap, const char *s) {
    size_t d = strlen(dst), n = strlen(s);
    if (d + n + 1 > cap) return;
    memcpy(dst + d, s, n + 1);
}

/* Reads a whole file of the app's namespace into buf. Returns the length, or a negative error. */
static int32_t read_file(const char *path, char *buf, size_t cap) {
    int32_t fd = pal_open(path, (uint32_t)strlen(path), PAL_O_READ);
    if (fd < 0) return fd;
    size_t n = 0;
    while (n < cap) {
        int32_t k = pal_read(fd, buf + n, (uint32_t)(cap - n));
        if (k <= 0) break;
        n += (size_t)k;
    }
    pal_close(fd);
    return (int32_t)n;
}

static int append_file(const char *path, const char *text, size_t n) {
    int32_t fd = pal_open(path, (uint32_t)strlen(path), PAL_O_WRITE | PAL_O_CREATE | PAL_O_APPEND);
    if (fd < 0) return -1;
    int32_t w = pal_write(fd, text, (uint32_t)n);
    pal_close(fd);
    return w == (int32_t)n ? 0 : -1;
}

static const char *kind_name(const char *alg) {
    if (strcmp(alg, "ssh-ed25519") == 0) return "ED25519";
    if (strcmp(alg, "ecdsa-sha2-nistp256") == 0) return "ECDSA";
    return "RSA";
}

/* Host key check (after the signature was verified): known, new (asks), or changed (refused). */
static int check_host(void *ctx, const char *host, uint16_t port, const char *alg, const uint8_t *blob, size_t n) {
    (void)ctx;
    int32_t kl = read_file(KNOWN_HOSTS, known, sizeof known);
    if (kl < 0) kl = 0; /* no file yet: no host is known */
    if ((size_t)kl >= sizeof known) { /* the rest would be unseen: a changed key there could look new */
        say_err("ssh: " KNOWN_HOSTS " is larger than 16 KB; refusing to connect\n");
        return -1;
    }
    int state = pssh_hosts_lookup(&S.ops, known, (size_t)kl, host, port, alg, blob, n);
    if (state == PSSH_HOST_FOUND) return 0;
    char fp[80], msg[512];
    pssh_fingerprint(&S.ops, blob, n, fp, sizeof fp);
    if (state == PSSH_HOST_CHANGED) {
        snprintf(msg, sizeof msg,
                 "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                 "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!    @\n"
                 "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                 "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\n"
                 "The %s host key for %s has changed (%s).\n"
                 "The entry in " KNOWN_HOSTS " does not match it.\n"
                 "Host key verification failed.\n",
                 kind_name(alg), host, fp);
        say_err(msg);
        return -1;
    }
    snprintf(msg, sizeof msg,
             "The authenticity of host '%s (%s:%u)' can't be established.\n"
             "%s key fingerprint is %s.\n"
             "Are you sure you want to continue connecting (yes/no)? ",
             host, host, (unsigned)port, kind_name(alg), fp);
    say(msg);
    char ans[16];
    int32_t r = pal_read(0, ans, sizeof ans - 1);
    if (r <= 0 || ans[0] != 'y' || ans[1] != 'e' || ans[2] != 's') {
        say("Host key verification failed.\n");
        return -1;
    }
    char line[512];
    if (pssh_hosts_line(host, port, alg, blob, n, line, sizeof line) < 0 ||
        append_file(KNOWN_HOSTS, line, strlen(line)) != 0) {
        say("ssh: cannot write " KNOWN_HOSTS "\n");
        return -1;
    }
    snprintf(msg, sizeof msg, "Warning: Permanently added '%s' (%s) to the list of known hosts.\n", host,
             kind_name(alg));
    say(msg);
    return 0;
}

/* ---- keystore subcommands ---- */

static int cmd_keygen(const char *name) {
    int32_t h = pal_key_generate(PAL_KEY_ED25519, name, (uint32_t)strlen(name));
    if (h < 0) {
        printf("ssh: cannot make key %s (error %ld)\n", name, (long)h);
        return 1;
    }
    printf("made key %s (ed25519)\n", name);
    return 0;
}

static int cmd_pub(const char *name) {
    int32_t h = pal_key_load(name, (uint32_t)strlen(name));
    if (h < 0) {
        printf("ssh: no key named %s\n", name);
        return 1;
    }
    uint8_t blob[160];
    int32_t bl = pal_key_public(h, blob, sizeof blob);
    if (bl <= 0) return 1;
    char alg[32] = {0};
    uint32_t al = ((uint32_t)blob[0] << 24) | ((uint32_t)blob[1] << 16) | ((uint32_t)blob[2] << 8) | blob[3];
    if (al >= sizeof alg) return 1;
    memcpy(alg, blob + 4, al);
    char b64[512];
    if (pssh_b64_encode(blob, (size_t)bl, b64, sizeof b64) == 0) return 1;
    printf("%s %s %s\n", alg, b64, name);
    return 0;
}

static int cmd_keys(void) {
    char out[1024];
    int32_t n = pal_key_list(out, sizeof out);
    if (n < 0) return 1;
    if (n == 0) say("no keys\n");
    else pal_write(1, out, (uint32_t)n);
    return 0;
}

static int cmd_import(const char *name, const char *file) {
    static char blob[4096];
    int32_t n = read_file(file, blob, sizeof blob);
    if (n <= 0) {
        printf("ssh: cannot read %s\n", file);
        return 1;
    }
    int32_t h = pal_key_import(blob, (uint32_t)n, "", 0, name, (uint32_t)strlen(name));
    if (h < 0) {
        printf("ssh: cannot import %s (error %ld; only unencrypted OpenSSH ed25519 keys)\n", file, (long)h);
        return 1;
    }
    printf("imported key %s\n", name);
    return 0;
}

static void usage(void) {
    say("usage: ssh [-p port] [-i key] [-l user] [-t] [user@]host [command]\n"
        "       ssh keygen [name] | pub [name] | keys | import name FILE\n");
}

/* ---- connection ---- */

static int run_session(const char *command) {
    int cols = 80, rows = 24;
    uint8_t sz[4];
    if (pal_tty_size(sz, sizeof sz) == 0) {
        cols = sz[0] | (sz[1] << 8);
        rows = sz[2] | (sz[3] << 8);
    }
    pal_tty_mode(PAL_TTY_RAW); /* Ctrl-C reaches the server as a byte; it is not a local stop */
    int rc = pssh_session_open(&S, command, cols, rows);
    if (rc == PSSH_OK) rc = pssh_session_run(&S);
    char msg[200];
    if (rc == PSSH_E_QUIT) return 130;
    if (rc == PSSH_E_EOF) {
        snprintf(msg, sizeof msg, "Connection to %s closed by remote host.\n", S.host);
        say_err(msg);
        return 255;
    }
    if (rc < 0) {
        snprintf(msg, sizeof msg, "ssh: %s\n", pssh_strerror(rc));
        say_err(msg);
        return 255;
    }
    if (S.want_pty) {
        snprintf(msg, sizeof msg, "Connection to %s closed.\n", S.host);
        say_err(msg);
    }
    return rc;
}

static int connect_and_run(const char *user, const char *host, uint16_t port, const char *key, int want_pty,
                           const char *command) {
    char msg[200];
    int32_t sock = pal_tcp_connect(host, (int32_t)port, 15000);
    if (sock < 0) {
        snprintf(msg, sizeof msg, "ssh: connect to host %s port %u: %s\n", host, (unsigned)port,
                 sock == PAL_ERR_REFUSED ? "Connection refused"
                 : sock == PAL_ERR_TIMEOUT ? "Connection timed out"
                 : sock == PAL_ERR_NOENT ? "Name or service not known"
                                         : "No route to host");
        say_err(msg);
        return 255;
    }
    if (pssh_pal_init(&P, sock) != PSSH_OK) return 255;
    pssh_ops_t ops;
    pssh_pal_ops(&ops, &P);
    if (pssh_init(&S, &ops) != PSSH_OK) return 255;
    strcpy(S.user, user);
    strcpy(S.host, host);
    S.port = port;
    strcpy(S.key_name, key);
    S.want_pty = want_pty;
    S.hostkey_check = check_host;

    int rc = pssh_handshake(&S, 20000);
    if (rc == PSSH_E_QUIT) return 130;
    if (rc != PSSH_OK) {
        snprintf(msg, sizeof msg, "ssh: %s: %s\n", host, pssh_strerror(rc));
        say_err(msg);
        return 255;
    }
    rc = pssh_authenticate(&S, 30000);
    if (rc == PSSH_E_QUIT) return 130;
    if (rc != PSSH_OK) {
        snprintf(msg, sizeof msg, "%s@%s: Permission denied (%s).\n", user, host,
                 rc == PSSH_E_EOF ? "the server closed the connection" : "publickey,keyboard-interactive,password");
        say_err(msg);
        return 255;
    }
    if (S.banner[0]) {
        say_err(S.banner);
        say_err("\n");
    }
    return run_session(command);
}

void _start(void) {
    char w[256], cmd[512] = {0}, host[PSSH_HOST_MAX] = {0}, user[PSSH_NAME_MAX] = {0}, key[PSSH_NAME_MAX] = {0};
    int argc = pal_argc();
    if (argc >= 2 && word(1, w, sizeof w)) {
        if (strcmp(w, "keygen") == 0 || strcmp(w, "pub") == 0) {
            char name[PSSH_NAME_MAX] = DEFAULT_KEY;
            if (argc >= 3) word(2, name, sizeof name);
            pal_exit(strcmp(w, "pub") == 0 ? cmd_pub(name) : cmd_keygen(name));
        }
        if (strcmp(w, "keys") == 0) pal_exit(cmd_keys());
        if (strcmp(w, "import") == 0) {
            char name[PSSH_NAME_MAX], file[256];
            if (argc < 4 || !word(2, name, sizeof name) || !word(3, file, sizeof file)) {
                usage();
                pal_exit(2);
            }
            pal_exit(cmd_import(name, file));
        }
    }
    uint16_t port = 22;
    int want_pty = -1, i = 1;
    while (i < argc && word(i, w, sizeof w) && w[0] == '-' && w[1]) {
        if (strcmp(w, "-p") == 0 && i + 1 < argc && word(i + 1, w, sizeof w)) {
            long v = strtol(w, NULL, 10);
            if (v <= 0 || v > 65535) {
                usage();
                pal_exit(255);
            }
            port = (uint16_t)v;
            i += 2;
        } else if (strcmp(w, "-i") == 0 && i + 1 < argc && word(i + 1, key, sizeof key)) {
            i += 2;
        } else if (strcmp(w, "-l") == 0 && i + 1 < argc && word(i + 1, user, sizeof user)) {
            i += 2;
        } else if (strcmp(w, "-t") == 0) {
            want_pty = 1;
            i++;
        } else {
            usage();
            pal_exit(255);
        }
    }
    if (i >= argc || !word(i, w, sizeof w)) {
        usage();
        pal_exit(255);
    }
    char *at = strchr(w, '@');
    if (at) {
        *at = 0;
        if (!user[0] && strlen(w) < sizeof user) strcpy(user, w);
        if (strlen(at + 1) < sizeof host) strcpy(host, at + 1);
    } else if (strlen(w) < sizeof host) {
        strcpy(host, w);
    }
    if (strlen(user) >= sizeof user || strlen(key) >= sizeof key) {
        usage();
        pal_exit(255);
    }
    if (!user[0] && !(pal_getenv("USER", user, sizeof user) > 0)) strcpy(user, "palanu");
    if (!key[0]) strcpy(key, DEFAULT_KEY);
    for (i = i + 1; i < argc; i++) {
        char part[256];
        if (!word(i, part, sizeof part)) break;
        if (cmd[0]) append(cmd, sizeof cmd, " ");
        append(cmd, sizeof cmd, part);
    }
    if (want_pty < 0) want_pty = cmd[0] ? 0 : 1;
    if (!host[0]) {
        usage();
        pal_exit(255);
    }
    pal_exit(connect_and_run(user, host, port, key, want_pty, cmd));
}
