/* The proc module of the palanu API (api/palanu/proc.h): exit, argc, argv, the environment, and the children an app
 * starts. A child is a registered command run as a process of its own; it shares the app's stdio and joins its process
 * group. An app can wait for and signal only the children it started, and the system reaps the ones it left behind. */
#include <stdint.h>
#include <string.h>

#include "../src/api_ctx.h"
#include "../src/api_impl.h"
#include "nema/nema.h"

#define ARGS_MAX 255u /* bytes of argument words, NULs included */
#define WORDS_MAX 15u /* argument words after the command name */

/* The PAL_SPAWN_DETACH and PAL_SIG_* values of api/palanu/proc.h, as numbers. */
#define SPAWN_DETACH 0x01u

void psys_api_exit(api_ctx_t *c, int32_t code) { api_exit(c, code); }

int32_t psys_api_argc(api_ctx_t *c) { return api_argc(c); }

int32_t psys_api_argv(api_ctx_t *c, int32_t i, void *buf, uint32_t cap) {
    const char *s = api_argv(c, i);
    if (!s) return NEMA_ERR_RANGE;
    size_t n = strlen(s);
    if (n + 1 > cap) return NEMA_ERR_RANGE;
    memcpy(buf, s, n + 1);
    return (int32_t)n;
}

/* The slot of child in the app's list, or -1. */
static int kid_index(api_ctx_t *c, int32_t child) {
    if (child <= 0) return -1;
    api_ext_t *x = api_ext(c);
    for (int i = 0; i < API_KIDS_MAX; i++)
        if (x->kids[i] == child) return i;
    return -1;
}

/* Words are NUL-terminated and follow each other; the last one may be left open at args_len (the buffer is terminated). */
int32_t psys_api_spawn(api_ctx_t *c, const char *app, const char *args, uint32_t args_len, int32_t flags) {
    if (flags < 0 || ((uint32_t)flags & ~SPAWN_DETACH)) return NEMA_ERR_INVAL;
    if (args_len > ARGS_MAX) return NEMA_ERR_RANGE;
    api_ext_t *x = api_ext(c);
    int slot = -1;
    for (int i = 0; i < API_KIDS_MAX && slot < 0; i++)
        if (x->kids[i] == 0) slot = i;
    if (slot < 0) return NEMA_ERR_FULL;
    char buf[ARGS_MAX + 1];
    if (args_len) memcpy(buf, args, args_len);
    buf[args_len] = 0;
    const char *argv[WORDS_MAX + 2]; /* the command name, then the words */
    int argc = 0;
    argv[argc++] = app;
    for (size_t i = 0; i < args_len;) {
        if ((unsigned)argc > WORDS_MAX) return NEMA_ERR_RANGE;
        argv[argc++] = buf + i;
        i += strlen(buf + i) + 1;
    }
    nema_proc_t *self = nema_self();
    nema_spawn_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.name = app;
    sp.argc = argc;
    sp.argv = argv;
    sp.pgid = self ? self->pgid : 0; /* join the app's group: Ctrl-C reaches the child too */
    sp.flags = ((uint32_t)flags & SPAWN_DETACH) ? NEMA_SPAWN_DETACH : 0;
    nema_pid_t pid = 0;
    nema_err_t r = nema_proc_spawn(&sp, &pid);
    if (r != NEMA_OK) return r;
    x->kids[slot] = pid;
    return pid;
}

int32_t psys_api_child_wait(api_ctx_t *c, int32_t child, void *code_out, uint32_t len, int32_t timeout_ms) {
    if (len < 4) return NEMA_ERR_INVAL;
    int k = kid_index(c, child);
    if (k < 0) return NEMA_ERR_NOENT;
    int code = 0;
    nema_err_t r = nema_proc_wait((nema_pid_t)child, &code, timeout_ms);
    if (r == NEMA_ERR_TIMEOUT) return 0;
    if (r == NEMA_OK) {
        api_put32(code_out, (uint32_t)code);
        api_ext(c)->kids[k] = 0;
        return 1;
    }
    if (r == NEMA_ERR_NOENT) { /* already reaped (a detached child): nothing to wait for */
        api_ext(c)->kids[k] = 0;
        return r;
    }
    if (r == NEMA_ERR_INTR || r == NEMA_ERR_CANCELED) api_stop(c, "interrupted");
    return r;
}

/* Maps the PAL_SIG_* values of api/palanu/proc.h (INT 1, TERM 2, KILL 4, HUP 8) to the system's signal bits. */
int32_t psys_api_signal(api_ctx_t *c, int32_t child, int32_t sig) {
    uint32_t bit;
    switch (sig) {
    case 0x01: bit = NEMA_SIG_INT; break;
    case 0x02: bit = NEMA_SIG_TERM; break;
    case 0x04: bit = NEMA_SIG_KILL; break;
    case 0x08: bit = NEMA_SIG_HUP; break;
    default: return NEMA_ERR_INVAL;
    }
    if (kid_index(c, child) < 0) return NEMA_ERR_NOENT;
    return nema_proc_signal((nema_pid_t)child, bit);
}

/* A minimal environment: HOME, USER and TERM are fixed, PWD is the app's working directory. */
int32_t psys_api_getenv(api_ctx_t *c, const char *key, void *buf, uint32_t cap) {
    (void)c;
    nema_proc_t *self = nema_self();
    const char *val;
    if (strcmp(key, "HOME") == 0) val = "/home";
    else if (strcmp(key, "USER") == 0) val = "palanu";
    else if (strcmp(key, "TERM") == 0) val = "xterm-256color";
    else if (strcmp(key, "PWD") == 0) val = self ? self->cwd : "/";
    else return NEMA_ERR_NOENT;
    size_t n = strlen(val);
    if (n + 1 > cap) return NEMA_ERR_RANGE;
    memcpy(buf, val, n + 1);
    return (int32_t)n;
}

/* Called when the app ends: the children it left running are reaped by the system when they end, and the ones that
 * already ended are reaped now (nobody will wait for them). */
void api_kids_release(api_ctx_t *c) {
    api_ext_t *x = api_ext(c);
    for (int i = 0; i < API_KIDS_MAX; i++) {
        if (x->kids[i] == 0) continue;
        nema_proc_detach((nema_pid_t)x->kids[i]);
        x->kids[i] = 0;
    }
}
