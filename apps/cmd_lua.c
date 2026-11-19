/* lua: Lua 5.4 on the device (ADR-0019). `lua FILE [ARGS]` runs a script, `lua -e CODE` a line, and `lua` alone is the prompt: an
 * expression prints its value, a statement that is not finished continues on the next line, Ctrl-D leaves. Ctrl-C stops a script.
 * The runtime is lua_rt.c; docs/LUA.md (also `man lua`) lists what the pal module offers. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "lua_rt.h"

#define LINE_MAX_LUA 512

static void to_tty(void *ctx, const char *s, size_t n) {
    (void)ctx;
    (void)nema_write(1, s, n);
}

/* One line of the prompt: added to the statement so far, run when it is complete. */
static void repl_line(lua_rt_t *rt, char *buf, size_t *len, const char *line, size_t ln) {
    if (*len + ln + 2 > 4 * LINE_MAX_LUA) { /* too long for one statement: start again */
        pal_eprintf("lua: the statement is too long\n");
        *len = 0;
        return;
    }
    if (*len) buf[(*len)++] = '\n';
    memcpy(buf + *len, line, ln);
    *len += ln;
    buf[*len] = 0;
    if (!lua_rt_complete(rt, buf, *len)) return; /* the next line finishes it */
    (void)lua_rt_run(rt, buf, *len, "=stdin", 1);
    *len = 0;
}

static int repl(lua_rt_t *rt) {
    pal_printf("Lua 5.4 on PalanuOS: an expression prints its value; Ctrl-D leaves, man lua has the pal module\n");
    char *buf = nema_alloc(NEMA_MEM_ANY, nema_self()->owner, 4 * LINE_MAX_LUA);
    if (!buf) return 1;
    size_t len = 0;
    for (;;) {
        pal_printf(len ? ">> " : "> ");
        char chunk[LINE_MAX_LUA];
        int n = nema_read(0, chunk, sizeof(chunk) - 1);
        if (n <= 0) break;
        chunk[n] = 0;
        /* a read may hold several lines (pasted text): each is one line of the prompt */
        for (char *p = chunk; *p;) {
            char *nl = p + strcspn(p, "\r\n");
            bool ended = *nl != 0;
            *nl = 0;
            if (ended || *p) repl_line(rt, buf, &len, p, strlen(p));
            if (!ended) break;
            p = nl + 1;
            while (*p == '\r' || *p == '\n') p++;
        }
    }
    pal_printf("\n");
    nema_free(buf);
    return 0;
}

static int cmd_lua(int argc, char **argv) {
    lua_rt_t *rt = lua_rt_new(0, to_tty, NULL);
    if (!rt) {
        pal_eprintf("lua: not enough memory\n");
        return 1;
    }
    int rc = 0;
    if (argc >= 3 && !strcmp(argv[1], "-e")) {
        lua_rt_args(rt, argc - 2, argv + 2);
        rc = lua_rt_run(rt, argv[2], strlen(argv[2]), "=(command line)", 0);
    } else if (argc >= 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
        pal_printf("Lua 5.4.7 (PalanuOS)\n");
    } else if (argc >= 2) {
        int fd = nema_open(argv[1], NEMA_O_READ);
        if (fd < 0) {
            pal_eprintf("lua: %s: %s\n", argv[1], pal_strerror(fd));
            lua_rt_free(rt);
            return 1;
        }
        size_t n;
        char *code = pal_read_all(fd, &n);
        nema_close(fd);
        if (!code) {
            pal_eprintf("lua: %s: not enough memory\n", argv[1]);
            lua_rt_free(rt);
            return 1;
        }
        lua_rt_args(rt, argc - 1, argv + 1);
        char name[80];
        snprintf(name, sizeof(name), "@%s", argv[1]);
        const char *body = code;
        if (n >= 2 && code[0] == '#' && code[1] == '!') { /* a #! first line, as lua.c skips it */
            const char *nl = memchr(code, '\n', n);
            body = nl ? nl : code + n;
        }
        rc = lua_rt_run(rt, body, n - (size_t)(body - code), name, 0);
        nema_free(code);
    } else {
        rc = repl(rt);
    }
    lua_rt_free(rt);
    return rc;
}

static const nema_app_t LUA_APP = {"lua", "Lua 5.4: lua [FILE [ARGS] | -e CODE] (alone: the prompt)", cmd_lua, 24 * 1024, 0};

void pal_apps_register_lua(void);
void pal_apps_register_lua(void) { nema_app_register(&LUA_APP); }
