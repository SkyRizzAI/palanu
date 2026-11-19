/* scp and sftp: files to and from an SSH server over the SSH File Transfer Protocol (libssh's sftp; ADR-0017). They share the ssh
 * command's connection (ssh_session.h): the same keys, known_hosts and /home/.ssh/config profiles. scp copies one file either way;
 * sftp is the interactive client (ls, cd, get, put, mkdir, rm, rename). Built where libssh has its SFTP part (PAL_HAVE_SFTP). */
#if __has_include(<libssh/libssh.h>) && defined(PAL_HAVE_SFTP)

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libssh/sftp.h>

#include "libapp.h"
#include "pal/sysinfo.h"
#include "ssh_session.h"

#define CHUNK 8192
#define PATHLEN 256

typedef struct {
    const char *keys[SSH_MAX_KEYS];
    int nkeys, port;
    bool port_set;
    const char *arg[3]; /* what is left after the options */
    int narg;
} opts_t;

/* -P PORT (scp) or -p PORT, -i KEYFILE; the rest in arg. false on a bad option. */
static bool parse(int argc, char **argv, opts_t *o) {
    memset(o, 0, sizeof(*o));
    o->port = 22;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-P") || !strcmp(argv[i], "-p")) && i + 1 < argc) o->port = atoi(argv[++i]), o->port_set = true;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            if (o->nkeys < SSH_MAX_KEYS) o->keys[o->nkeys++] = argv[++i];
            else i++;
        } else if (argv[i][0] == '-') return false;
        else if (o->narg < 3) o->arg[o->narg++] = argv[i];
    }
    return true;
}

static const char *sftp_why(sftp_session sf) {
    switch (sftp_get_error(sf)) {
    case SSH_FX_NO_SUCH_FILE: return "no such file";
    case SSH_FX_PERMISSION_DENIED: return "permission denied";
    case SSH_FX_FILE_ALREADY_EXISTS: return "already exists";
    case SSH_FX_FAILURE: return "the server refused it";
    case SSH_FX_CONNECTION_LOST: return "connection lost";
    default: return "failed";
    }
}

/* SFTP needs the session blocking; ssh_open leaves it non-blocking for the interactive shell. */
static sftp_session start(conn_t *c, const char *who) {
    ssh_set_blocking(c->s, 1);
    sftp_session sf = sftp_new(c->s);
    if (!sf) {
        pal_eprintf("%s: %s\n", who, ssh_get_error(c->s));
        return NULL;
    }
    if (sftp_init(sf) != SSH_OK) {
        pal_eprintf("%s: the server does not offer SFTP: %s\n", who, ssh_get_error(c->s));
        sftp_free(sf);
        return NULL;
    }
    return sf;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}

static bool remote_is_dir(sftp_session sf, const char *path) {
    sftp_attributes a = sftp_stat(sf, path);
    bool dir = a && a->type == SSH_FILEXFER_TYPE_DIRECTORY;
    if (a) sftp_attributes_free(a);
    return dir;
}

static bool local_is_dir(const char *path) {
    nema_stat_t st;
    return nema_fs_stat(path, &st) == NEMA_OK && st.type == NEMA_T_DIR;
}

/* dir + "/" + name, or path itself when it is not a folder. */
static void into(char *out, size_t cap, const char *path, bool is_dir, const char *name) {
    size_t n = strlen(path);
    if (is_dir) snprintf(out, cap, "%s%s%s", path, n && path[n - 1] == '/' ? "" : "/", name);
    else snprintf(out, cap, "%s", path);
}

static int get_file(sftp_session sf, const char *who, const char *remote, const char *local) {
    char dst[PATHLEN];
    into(dst, sizeof(dst), local, local_is_dir(local), base_name(remote));
    sftp_file f = sftp_open(sf, remote, O_RDONLY, 0);
    if (!f) {
        pal_eprintf("%s: %s: %s\n", who, remote, sftp_why(sf));
        return 1;
    }
    int fd = nema_open(dst, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC);
    if (fd < 0) {
        pal_eprintf("%s: %s: %s\n", who, dst, pal_strerror(fd));
        sftp_close(f);
        return 1;
    }
    char *buf = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, CHUNK);
    unsigned long total = 0;
    int rc = buf ? 0 : 1;
    uint64_t t0 = pal_sys_uptime_us();
    while (buf && !nema_interrupted()) {
        ssize_t n = sftp_read(f, buf, CHUNK);
        if (n < 0) {
            pal_eprintf("%s: %s: %s\n", who, remote, sftp_why(sf));
            rc = 1;
            break;
        }
        if (n == 0) break;
        if (nema_write(fd, buf, (size_t)n) != (int)n) {
            pal_eprintf("%s: %s: cannot write (full?)\n", who, dst);
            rc = 1;
            break;
        }
        total += (unsigned long)n;
    }
    nema_free(buf);
    nema_close(fd);
    sftp_close(f);
    if (rc == 0) {
        unsigned long ms = (unsigned long)((pal_sys_uptime_us() - t0) / 1000u);
        pal_printf("%s -> %s: %lu bytes in %lu.%lu s\n", remote, dst, total, ms / 1000u, ms % 1000u / 100u);
    }
    return rc;
}

static int put_file(sftp_session sf, const char *who, const char *local, const char *remote) {
    char dst[PATHLEN];
    into(dst, sizeof(dst), remote[0] ? remote : ".", !remote[0] || remote_is_dir(sf, remote), base_name(local));
    int fd = nema_open(local, NEMA_O_READ);
    if (fd < 0) {
        pal_eprintf("%s: %s: %s\n", who, local, pal_strerror(fd));
        return 1;
    }
    sftp_file f = sftp_open(sf, dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!f) {
        pal_eprintf("%s: %s: %s\n", who, dst, sftp_why(sf));
        nema_close(fd);
        return 1;
    }
    char *buf = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, CHUNK);
    unsigned long total = 0;
    int rc = buf ? 0 : 1;
    uint64_t t0 = pal_sys_uptime_us();
    while (buf && !nema_interrupted()) {
        int n = nema_read(fd, buf, CHUNK);
        if (n < 0) {
            pal_eprintf("%s: %s: %s\n", who, local, pal_strerror(n));
            rc = 1;
            break;
        }
        if (n == 0) break;
        if (sftp_write(f, buf, (size_t)n) != n) {
            pal_eprintf("%s: %s: %s\n", who, dst, sftp_why(sf));
            rc = 1;
            break;
        }
        total += (unsigned long)n;
    }
    nema_free(buf);
    nema_close(fd);
    sftp_close(f);
    if (rc == 0) {
        unsigned long ms = (unsigned long)((pal_sys_uptime_us() - t0) / 1000u);
        pal_printf("%s -> %s: %lu bytes in %lu.%lu s\n", local, dst, total, ms / 1000u, ms % 1000u / 100u);
    }
    return rc;
}

/* "[user@]host:path": the colon comes before any slash. Splits it into target and path. */
static bool remote_spec(const char *a, char *target, size_t tcap, const char **path) {
    const char *colon = strchr(a, ':'), *slash = strchr(a, '/');
    if (!colon || colon == a || (slash && slash < colon)) return false;
    snprintf(target, tcap, "%.*s", (int)(colon - a), a);
    *path = colon + 1;
    return true;
}

/* scp [-P port] [-i key] SRC DST: one of them [user@]host:path. */
static int cmd_scp(int argc, char **argv) {
    opts_t o;
    char target[96];
    const char *rpath = NULL;
    bool down = false;
    if (parse(argc, argv, &o) && o.narg == 2) {
        if (remote_spec(o.arg[0], target, sizeof(target), &rpath)) down = true;
        else if (!remote_spec(o.arg[1], target, sizeof(target), &rpath)) rpath = NULL;
    }
    if (!rpath) {
        pal_eprintf("usage: scp [-P port] [-i key] [user@]host:path LOCAL    or    scp [-P port] [-i key] LOCAL [user@]host:path\n");
        return 2;
    }
    conn_t c;
    int rc = ssh_open(&c, "scp", target, o.port, o.port_set, "palanu", false, o.keys, o.nkeys);
    if (rc) return rc;
    sftp_session sf = start(&c, "scp");
    rc = 1;
    if (sf) {
        if (down) rc = get_file(sf, "scp", rpath[0] ? rpath : ".", o.arg[1]);
        else rc = put_file(sf, "scp", o.arg[0], rpath);
        sftp_free(sf);
    }
    ssh_close(&c);
    return rc;
}

/* ---- sftp: the interactive client ---- */

/* A path against the remote folder: absolute stays, relative joins it. */
static void rjoin(char *out, size_t cap, const char *cwd, const char *p) {
    if (p[0] == '/') snprintf(out, cap, "%s", p);
    else snprintf(out, cap, "%s/%s", cwd, p);
}

static void sftp_ls(sftp_session sf, const char *path) {
    sftp_dir d = sftp_opendir(sf, path);
    if (!d) {
        pal_eprintf("ls: %s: %s\n", path, sftp_why(sf));
        return;
    }
    sftp_attributes a;
    while ((a = sftp_readdir(sf, d)) != NULL) {
        if (strcmp(a->name, ".") && strcmp(a->name, ".."))
            pal_printf("%c %10llu  %s%s\n", a->type == SSH_FILEXFER_TYPE_DIRECTORY ? 'd' : '-', (unsigned long long)a->size, a->name,
                       a->type == SSH_FILEXFER_TYPE_DIRECTORY ? "/" : "");
        sftp_attributes_free(a);
    }
    sftp_closedir(d);
}

/* Words of a line, with "double quotes" around one that holds spaces. */
static int words(char *line, char **w, int max) {
    int n = 0;
    for (char *p = line; *p && n < max;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char end = ' ';
        if (*p == '"') end = '"', p++;
        w[n++] = p;
        while (*p && *p != end && !(end == ' ' && *p == '\t')) p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static const char HELP[] = "ls [path]  cd path  pwd  get remote [local]  put local [remote]  mkdir path  rmdir path  rm path\n"
                           "rename from to  lcd path  lpwd  help  exit\n";

static void shell(sftp_session sf, const char *target) {
    char *cwd = sftp_canonicalize_path(sf, ".");
    char rcwd[PATHLEN];
    snprintf(rcwd, sizeof(rcwd), "%s", cwd ? cwd : "/");
    ssh_string_free_char(cwd);
    pal_printf("connected to %s; help lists the commands\n", target);
    char line[PATHLEN + 64], a[PATHLEN], b[PATHLEN];
    for (;;) {
        pal_printf("sftp> ");
        int n = nema_read(0, line, sizeof(line) - 1);
        if (n <= 0 || nema_interrupted()) break;
        line[n] = 0;
        line[strcspn(line, "\r\n")] = 0;
        char *w[4];
        int nw = words(line, w, 4);
        if (nw == 0) continue;
        const char *cmd = w[0];
        if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit") || !strcmp(cmd, "bye")) break;
        else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) pal_printf("%s", HELP);
        else if (!strcmp(cmd, "pwd")) pal_printf("%s\n", rcwd);
        else if (!strcmp(cmd, "lpwd")) pal_printf("%s\n", nema_self()->cwd);
        else if (!strcmp(cmd, "lcd") && nw > 1) {
            nema_err_t e = nema_set_cwd(w[1]);
            if (e != NEMA_OK) pal_eprintf("lcd: %s: %s\n", w[1], pal_strerror(e));
        } else if (!strcmp(cmd, "ls")) {
            rjoin(a, sizeof(a), rcwd, nw > 1 ? w[1] : ".");
            sftp_ls(sf, a);
        } else if (!strcmp(cmd, "cd") && nw > 1) {
            rjoin(a, sizeof(a), rcwd, w[1]);
            char *real = sftp_canonicalize_path(sf, a);
            if (real && remote_is_dir(sf, real)) snprintf(rcwd, sizeof(rcwd), "%s", real);
            else pal_eprintf("cd: %s: not a folder\n", w[1]);
            ssh_string_free_char(real);
        } else if (!strcmp(cmd, "get") && nw > 1) {
            rjoin(a, sizeof(a), rcwd, w[1]);
            (void)get_file(sf, "get", a, nw > 2 ? w[2] : ".");
        } else if (!strcmp(cmd, "put") && nw > 1) {
            if (nw > 2) rjoin(a, sizeof(a), rcwd, w[2]);
            else snprintf(a, sizeof(a), "%s", rcwd);
            (void)put_file(sf, "put", w[1], a);
        } else if ((!strcmp(cmd, "mkdir") || !strcmp(cmd, "rmdir") || !strcmp(cmd, "rm")) && nw > 1) {
            rjoin(a, sizeof(a), rcwd, w[1]);
            int r = !strcmp(cmd, "mkdir") ? sftp_mkdir(sf, a, 0755) : !strcmp(cmd, "rmdir") ? sftp_rmdir(sf, a) : sftp_unlink(sf, a);
            if (r != 0) pal_eprintf("%s: %s: %s\n", cmd, w[1], sftp_why(sf));
        } else if (!strcmp(cmd, "rename") && nw > 2) {
            rjoin(a, sizeof(a), rcwd, w[1]);
            rjoin(b, sizeof(b), rcwd, w[2]);
            if (sftp_rename(sf, a, b) != 0) pal_eprintf("rename: %s\n", sftp_why(sf));
        } else {
            pal_eprintf("%s: unknown or missing argument; help lists the commands\n", cmd);
        }
    }
}

/* sftp [-P port] [-i key] [user@]host */
static int cmd_sftp(int argc, char **argv) {
    opts_t o;
    if (!parse(argc, argv, &o) || o.narg != 1) {
        pal_eprintf("usage: sftp [-P port] [-i key] [user@]host   (host may be a Host in /home/.ssh/config)\n");
        return 2;
    }
    conn_t c;
    int rc = ssh_open(&c, "sftp", o.arg[0], o.port, o.port_set, "palanu", false, o.keys, o.nkeys);
    if (rc) return rc;
    sftp_session sf = start(&c, "sftp");
    rc = 1;
    if (sf) {
        shell(sf, o.arg[0]);
        sftp_free(sf);
        rc = 0;
    }
    ssh_close(&c);
    return rc;
}

static const nema_app_t SFTP_APPS[] = {
    {"scp", "copy a file to or from an SSH server: scp [-P port] SRC DST (one is [user@]host:path)", cmd_scp, 20 * 1024, 0},
    {"sftp", "files on an SSH server: sftp [-P port] [-i key] [user@]host", cmd_sftp, 20 * 1024, 0},
};

void pal_apps_register_sftp(void);
void pal_apps_register_sftp(void) {
    for (size_t i = 0; i < sizeof(SFTP_APPS) / sizeof(SFTP_APPS[0]); i++) nema_app_register(&SFTP_APPS[i]);
}

#else

void pal_apps_register_sftp(void);
void pal_apps_register_sftp(void) {}

#endif
