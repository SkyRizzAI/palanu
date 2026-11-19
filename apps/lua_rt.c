/* The Lua runtime (lua_rt.h). Lua's own io, os and package libraries use the C library's files and dlopen, which are not the
 * system's: in their place print, io.write, io.read, loadfile and dofile go through Nema, and the pal module reaches the system. */
#include "lua_rt.h"

#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "libapp.h"
#include "lua.h"
#include "lualib.h"
#include "pal/board.h"
#include "pal/music.h"
#include "pal/notify.h"
#include "pal/settings.h"
#include "pal/sysinfo.h"
#include "pal/version.h"

#define MEM_DEFAULT (1024u * 1024u)
#define HOOK_EVERY 4000 /* instructions between looks at Ctrl-C */

struct lua_rt {
    lua_State *L;
    size_t used, cap;
    lua_rt_out_t out;
    void *ctx;
    nema_owner_t owner;
};

/* ---- memory: Nema has no realloc, so a new block, the old bytes, and the old block freed ---- */
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    lua_rt_t *rt = (lua_rt_t *)ud;
    if (!ptr) osize = 0;
    if (nsize == 0) {
        nema_free(ptr);
        rt->used -= osize;
        return NULL;
    }
    if (rt->used - osize + nsize > rt->cap) return NULL; /* Lua raises "not enough memory" */
    void *p = nema_alloc(NEMA_MEM_EXTERNAL, rt->owner, nsize);
    if (!p) p = nema_alloc(NEMA_MEM_ANY, rt->owner, nsize);
    if (!p) return NULL;
    if (ptr) {
        memcpy(p, ptr, osize < nsize ? osize : nsize);
        nema_free(ptr);
    }
    rt->used = rt->used - osize + nsize;
    return p;
}

static lua_rt_t *rt_of(lua_State *L) {
    void *ud;
    lua_getallocf(L, &ud);
    return (lua_rt_t *)ud;
}

static void emit(lua_State *L, const char *s, size_t n) {
    lua_rt_t *rt = rt_of(L);
    if (rt->out) rt->out(rt->ctx, s, n);
}

static void hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (nema_interrupted()) luaL_error(L, "interrupted");
}

/* ---- print, io ---- */
static int l_print(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) emit(L, "\t", 1);
        emit(L, s, len);
        lua_pop(L, 1);
    }
    emit(L, "\n", 1);
    return 0;
}

static int l_io_write(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_checklstring(L, i, &len); /* numbers become text too, as io.write does */
        emit(L, s, len);
    }
    return 0;
}

/* io.read(["l" | "L" | "n"]): a line from the terminal (without its newline for "l"), or a number; nil at the end. */
static int l_io_read(lua_State *L) {
    const char *fmt = luaL_optstring(L, 1, "l");
    if (*fmt == '*') fmt++; /* Lua 5.1 style "*l" */
    char line[256];
    int n = nema_read(0, line, sizeof(line) - 1);
    if (n <= 0) {
        lua_pushnil(L);
        return 1;
    }
    line[n] = 0;
    if (*fmt == 'n') {
        if (lua_stringtonumber(L, line) == 0) lua_pushnil(L);
        return 1;
    }
    if (*fmt != 'L') line[strcspn(line, "\r\n")] = 0;
    lua_pushstring(L, line);
    return 1;
}

/* ---- files through Nema ---- */
/* Files through the object API, which needs no handle table (it works outside a process too). */
static char *read_file(const char *path, size_t *len, nema_err_t *err) {
    nema_obj_t *f;
    nema_stat_t st;
    *err = nema_fs_stat(path, &st);
    if (*err == NEMA_OK) *err = nema_fs_open(path, NEMA_O_READ, &f);
    if (*err != NEMA_OK) return NULL;
    nema_owner_t owner = nema_self() ? nema_self()->owner : NEMA_OWNER_KERNEL;
    char *t = nema_alloc(NEMA_MEM_EXTERNAL, owner, (size_t)st.size + 1);
    if (!t) t = nema_alloc(NEMA_MEM_ANY, owner, (size_t)st.size + 1);
    size_t n = 0;
    if (t) {
        int r;
        while (n < st.size && (r = nema_obj_read(f, t + n, st.size - n, -1)) > 0) n += (size_t)r;
        t[n] = 0;
    }
    nema_obj_unref(f);
    *err = t ? NEMA_OK : NEMA_ERR_NOMEM;
    *len = n;
    return t;
}

static int push_fail(lua_State *L, const char *path, nema_err_t e) {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", path, pal_strerror(e));
    return 2;
}

static int l_loadfile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    nema_err_t e;
    char *t = read_file(path, &len, &e);
    if (!t) return push_fail(L, path, e);
    char name[96];
    snprintf(name, sizeof(name), "@%s", path);
    int r = luaL_loadbufferx(L, t, len, name, "t");
    nema_free(t);
    if (r != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

static int l_dofile(lua_State *L) {
    lua_settop(L, 1);
    if (l_loadfile(L) != 1) return lua_error(L);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

/* ---- the pal module ---- */
static int l_sleep(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    while (ms > 0) {
        if (nema_interrupted()) return luaL_error(L, "interrupted");
        int step = ms > 50 ? 50 : (int)ms;
        nema_sleep_ms((uint32_t)step);
        ms -= step;
    }
    return 0;
}

static int l_uptime(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(pal_sys_uptime_us() / 1000u));
    return 1;
}

static int l_readfile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    nema_err_t e;
    char *t = read_file(path, &len, &e);
    if (!t) return push_fail(L, path, e);
    lua_pushlstring(L, t, len);
    nema_free(t);
    return 1;
}

static int l_writefile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    bool append = lua_toboolean(L, 3);
    nema_obj_t *f;
    nema_err_t e = nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | (append ? NEMA_O_APPEND : NEMA_O_TRUNC), &f);
    if (e != NEMA_OK) return push_fail(L, path, e);
    int w = len ? nema_obj_write(f, s, len, -1) : 0;
    nema_obj_unref(f);
    if (w != (int)len) return push_fail(L, path, w < 0 ? (nema_err_t)w : NEMA_ERR_NOSPC);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_ls(lua_State *L) {
    const char *path = luaL_optstring(L, 1, ".");
    lua_newtable(L);
    nema_dirent_t e;
    size_t i = 0;
    nema_err_t r = NEMA_OK;
    for (; (r = nema_fs_readdir(path, i, &e)) == NEMA_OK; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, e.name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, e.type == NEMA_T_DIR);
        lua_setfield(L, -2, "dir");
        lua_pushinteger(L, (lua_Integer)e.size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    if (i == 0 && r != NEMA_ERR_NOENT && r != NEMA_OK) {
        nema_stat_t st;
        if (nema_fs_stat(path, &st) != NEMA_OK) return push_fail(L, path, r);
    }
    return 1;
}

static int l_exists(lua_State *L) {
    nema_stat_t st;
    lua_pushboolean(L, nema_fs_stat(luaL_checkstring(L, 1), &st) == NEMA_OK);
    return 1;
}

static int l_remove(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    nema_err_t e = nema_fs_unlink(path);
    if (e != NEMA_OK) return push_fail(L, path, e);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_mkdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    nema_err_t e = nema_fs_mkdir(path);
    if (e != NEMA_OK) return push_fail(L, path, e);
    lua_pushboolean(L, 1);
    return 1;
}

/* pal.run(cmdline): runs it in the shell on this terminal and returns its exit status. */
static int l_run(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1);
    const char *av[] = {"sh", "-c", cmd};
    nema_spawn_t sp = {.name = "sh", .argc = 3, .argv = av, .pgid = nema_self()->pgid};
    nema_pid_t pid;
    nema_err_t e = nema_proc_spawn(&sp, &pid);
    if (e != NEMA_OK) return luaL_error(L, "pal.run: %s", pal_strerror(e));
    int code = 0;
    while (nema_proc_wait(pid, &code, 100) != NEMA_OK)
        if (nema_interrupted()) nema_proc_signal(pid, NEMA_SIG_INT);
    lua_pushinteger(L, code);
    return 1;
}

static int l_setting(lua_State *L) {
    char v[128];
    if (psys_settings_get(luaL_checkstring(L, 1), v, sizeof(v)) < 0) lua_pushnil(L);
    else lua_pushstring(L, v);
    return 1;
}

static int l_notify(lua_State *L) {
    const char *title = luaL_checkstring(L, 1), *text = luaL_optstring(L, 2, "");
    lua_pushinteger(L, psys_notify_post("lua", title, text, 0));
    return 1;
}

static int l_beep(lua_State *L) {
    pal_beep((int)luaL_optinteger(L, 1, 80));
    return 0;
}

static int l_tone(lua_State *L) { /* pal.tone(hz, ms): blocks for ms */
    pal_tone((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2));
    return 0;
}

static int l_play(lua_State *L) { /* pal.play(rtttl or a built-in name): true, or nil and why */
    const char *t = luaL_checkstring(L, 1);
    const char *b = pal_music_builtin(t);
    nema_err_t r = pal_music_play(b ? b : t);
    if (r == NEMA_ERR_INTR) return luaL_error(L, "interrupted");
    if (r != NEMA_OK) {
        lua_pushnil(L);
        lua_pushstring(L, r == NEMA_ERR_NOENT ? "no buzzer" : "not RTTTL");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg PAL_FNS[] = {{"sleep", l_sleep},   {"uptime", l_uptime}, {"readfile", l_readfile}, {"writefile", l_writefile},
                                   {"ls", l_ls},         {"exists", l_exists}, {"remove", l_remove},     {"mkdir", l_mkdir},
                                   {"run", l_run},       {"setting", l_setting}, {"notify", l_notify},   {"beep", l_beep},
                                   {"tone", l_tone},     {"play", l_play},
                                   {NULL, NULL}};

static int open_pal(lua_State *L) {
    luaL_newlib(L, PAL_FNS);
    lua_pushstring(L, PAL_OS_VERSION);
    lua_setfield(L, -2, "version");
    const char *board = pal_board_info()->name;
    lua_pushstring(L, board ? board : "");
    lua_setfield(L, -2, "board");
    return 1;
}

static int open_io(lua_State *L) {
    static const luaL_Reg IO[] = {{"write", l_io_write}, {"read", l_io_read}, {NULL, NULL}};
    luaL_newlib(L, IO);
    return 1;
}

lua_rt_t *lua_rt_new(size_t mem_cap, lua_rt_out_t out, void *ctx) {
    nema_owner_t owner = nema_self() ? nema_self()->owner : NEMA_OWNER_KERNEL;
    lua_rt_t *rt = nema_calloc(NEMA_MEM_ANY, owner, sizeof(*rt));
    if (!rt) return NULL;
    rt->cap = mem_cap ? mem_cap : MEM_DEFAULT;
    rt->out = out, rt->ctx = ctx, rt->owner = owner;
    rt->L = lua_newstate(l_alloc, rt);
    if (!rt->L) {
        nema_free(rt);
        return NULL;
    }
    lua_State *L = rt->L;
    static const luaL_Reg LIBS[] = {{LUA_GNAME, luaopen_base},          {LUA_COLIBNAME, luaopen_coroutine},
                                    {LUA_TABLIBNAME, luaopen_table},    {LUA_STRLIBNAME, luaopen_string},
                                    {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},
                                    {"io", open_io},                    {"pal", open_pal},
                                    {NULL, NULL}};
    for (const luaL_Reg *l = LIBS; l->name; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    lua_register(L, "print", l_print);
    lua_register(L, "loadfile", l_loadfile);
    lua_register(L, "dofile", l_dofile);
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
    return rt;
}

void lua_rt_free(lua_rt_t *rt) {
    if (!rt) return;
    lua_close(rt->L);
    nema_free(rt);
}

size_t lua_rt_mem(const lua_rt_t *rt) { return rt->used; }

void lua_rt_args(lua_rt_t *rt, int argc, char **argv) {
    lua_State *L = rt->L;
    lua_createtable(L, argc, 1);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
}

static int traceback(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (!msg) msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    luaL_traceback(L, L, msg, 1);
    return 1;
}

int lua_rt_run(lua_rt_t *rt, const char *code, size_t len, const char *name, int repl) {
    lua_State *L = rt->L;
    int base = lua_gettop(L);
    int r = LUA_ERRSYNTAX;
    if (repl) { /* an expression prints its value: try "return <line>" first */
        lua_pushfstring(L, "return %s", code);
        r = luaL_loadbufferx(L, lua_tostring(L, -1), lua_rawlen(L, -1), name, "t");
        lua_remove(L, -2);
        if (r != LUA_OK) lua_pop(L, 1);
    }
    if (r != LUA_OK) r = luaL_loadbufferx(L, code, len, name, "t"); /* text only: no precompiled chunks */
    if (r == LUA_OK) {
        lua_pushcfunction(L, traceback);
        lua_insert(L, -2);
        r = lua_pcall(L, 0, LUA_MULTRET, base + 1);
        lua_remove(L, base + 1);
    }
    if (r != LUA_OK) {
        size_t n;
        const char *msg = lua_tolstring(L, -1, &n);
        if (msg) {
            emit(L, msg, n);
            emit(L, "\n", 1);
        }
        lua_settop(L, base);
        return 1;
    }
    int nres = lua_gettop(L) - base;
    if (repl && nres > 0) { /* the values, as print shows them */
        lua_getglobal(L, "print");
        lua_insert(L, base + 1);
        lua_pcall(L, nres, 0, 0);
    }
    lua_settop(L, base);
    return 0;
}

int lua_rt_complete(lua_rt_t *rt, const char *code, size_t len) {
    lua_State *L = rt->L;
    int r = luaL_loadbufferx(L, code, len, "=stdin", "t");
    int incomplete = 0;
    if (r == LUA_ERRSYNTAX) { /* the parser stopped at the end of the text: more lines are coming */
        size_t n;
        const char *msg = lua_tolstring(L, -1, &n);
        incomplete = msg && n >= 5 && strcmp(msg + n - 5, "<eof>") == 0;
    }
    lua_pop(L, 1);
    return !incomplete;
}
