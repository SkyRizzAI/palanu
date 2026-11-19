/* The Lua runtime of the lua command (apps/cmd_lua.c, ADR-0019): Lua 5.4 with the base, string, table, math, utf8 and coroutine
 * libraries, its memory from nema_alloc (PSRAM first, a cap per state), Ctrl-C stopping a running script, and the pal module.
 * Output goes through a callback, so the tests read what a script prints. */
#ifndef PAL_LUA_RT_H
#define PAL_LUA_RT_H

#include <stddef.h>
#include <stdint.h>

typedef struct lua_rt lua_rt_t;
typedef void (*lua_rt_out_t)(void *ctx, const char *s, size_t n);

/* A new state; mem_cap bytes at most (0: 1 MB). out gets print, io.write and the error messages. */
lua_rt_t *lua_rt_new(size_t mem_cap, lua_rt_out_t out, void *ctx);
void lua_rt_free(lua_rt_t *rt);
/* Runs a chunk. 0 on success; 1 on an error, whose message went to out. A REPL line that is an expression prints its values. */
int lua_rt_run(lua_rt_t *rt, const char *code, size_t len, const char *name, int repl);
/* Sets the script's arguments: the global table arg, arg[0] the script, arg[1..n] the rest. */
void lua_rt_args(lua_rt_t *rt, int argc, char **argv);
/* 0 when code ends in the middle of a statement (the REPL asks for another line), 1 otherwise (complete, or a real error). */
int lua_rt_complete(lua_rt_t *rt, const char *code, size_t len);
/* Bytes the state holds now. */
size_t lua_rt_mem(const lua_rt_t *rt);

#endif
