/* The Lua runtime of the lua command (apps/lua_rt.c, ADR-0019): print and io.write reach the output, a REPL expression prints its
 * value, an unfinished statement is told apart from a real error, errors carry a traceback, arg holds the script's arguments,
 * the pal module reads and writes files through Nema, and the memory cap turns into Lua's own error. */
#include <string.h>

#include "../../apps/lua_rt.h"
#include "nema/nema.h"
#include "nema/vfs.h"
#include "test.h"

static char g_out[2048];
static size_t g_len;
static void out(void *ctx, const char *s, size_t n) {
    (void)ctx;
    if (g_len + n < sizeof(g_out)) memcpy(g_out + g_len, s, n), g_len += n;
    g_out[g_len] = 0;
}
static void clear(void) { g_len = 0, g_out[0] = 0; }
static int run(lua_rt_t *rt, const char *code, int repl) {
    clear();
    return lua_rt_run(rt, code, strlen(code), "=test", repl);
}

TEST(print_io_write_and_the_prompt) {
    lua_rt_t *rt = lua_rt_new(0, out, NULL);
    CHECK(rt != NULL);
    CHECK_EQ(run(rt, "print('a', 1, nil, true) io.write('x', 2, '\\n')", 0), 0);
    CHECK(!strcmp(g_out, "a\t1\tnil\ttrue\nx2\n"));
    CHECK_EQ(run(rt, "6 * 7", 1), 0); /* an expression at the prompt prints its value */
    CHECK(!strcmp(g_out, "42\n"));
    CHECK_EQ(run(rt, "y = 5", 1), 0); /* a statement prints nothing */
    CHECK(!strcmp(g_out, ""));
    CHECK_EQ(run(rt, "y + 1", 1), 0); /* and the state stays */
    CHECK(!strcmp(g_out, "6\n"));
    CHECK_EQ(run(rt, "string.format('%5.1f|%s', math.pi, utf8.char(65, 0x263A))", 1), 0);
    CHECK(!strcmp(g_out, "  3.1|A\xE2\x98\xBA\n"));
    lua_rt_free(rt);
}

TEST(unfinished_statements_and_errors) {
    lua_rt_t *rt = lua_rt_new(0, out, NULL);
    CHECK_EQ(lua_rt_complete(rt, "function f(x)", 13), 0);
    CHECK_EQ(lua_rt_complete(rt, "function f(x)\nreturn x end", 26), 1);
    CHECK_EQ(lua_rt_complete(rt, "1 +* 2", 6), 1); /* a real error is complete: it is shown, not continued */
    CHECK_EQ(run(rt, "nosuch()", 0), 1);
    CHECK(strstr(g_out, "attempt to call a nil value (global 'nosuch')") != NULL);
    CHECK(strstr(g_out, "stack traceback") != NULL);
    CHECK_EQ(run(rt, "print(pcall(error, 'boom'))", 0), 0); /* pcall works: longjmp inside the runtime */
    CHECK(!strcmp(g_out, "false\tboom\n"));
    CHECK_EQ(run(rt, "return 1", 0), 0);
    lua_rt_free(rt);
}

TEST(arguments_files_and_memory) {
    lua_rt_t *rt = lua_rt_new(256 * 1024, out, NULL);
    char *av[] = {"script.lua", "one", "two"};
    lua_rt_args(rt, 3, av);
    CHECK_EQ(run(rt, "print(arg[0], #arg, arg[2])", 0), 0);
    CHECK(!strcmp(g_out, "script.lua\t2\ttwo\n"));
    CHECK_EQ(run(rt, "assert(pal.writefile('/tmp/l.txt', 'hi')) assert(pal.writefile('/tmp/l.txt', '!', true)) print(pal.readfile('/tmp/l.txt'), pal.exists('/tmp/l.txt'))", 0), 0);
    CHECK(!strcmp(g_out, "hi!\ttrue\n"));
    CHECK_EQ(run(rt, "local n = 0 for _, e in ipairs(pal.ls('/tmp')) do if e.name == 'l.txt' then n = e.size end end print(n)", 0), 0);
    CHECK(!strcmp(g_out, "3\n"));
    CHECK_EQ(run(rt, "print(pal.readfile('/tmp/none'))", 0), 0);
    CHECK(strstr(g_out, "nil\t/tmp/none:") == g_out);
    CHECK_EQ(run(rt, "assert(pal.writefile('/tmp/m.lua', 'return 7 * 6')) print(dofile('/tmp/m.lua'))", 0), 0); /* dofile reads through Nema */
    CHECK(!strcmp(g_out, "42\n"));
    size_t before = lua_rt_mem(rt);
    CHECK_EQ(run(rt, "local t = {} for i = 1, 1e7 do t[i] = i end", 0), 1); /* past the 256 KB cap */
    CHECK(strstr(g_out, "not enough memory") != NULL);
    CHECK_EQ(run(rt, "collectgarbage() print('alive')", 0), 0); /* the state is still usable */
    CHECK(!strcmp(g_out, "alive\n"));
    CHECK(lua_rt_mem(rt) < before + 64 * 1024);
    lua_rt_free(rt);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(256u * 1024u));
    nema_fs_mkdir("/tmp");
    RUN(print_io_write_and_the_prompt);
    RUN(unfinished_statements_and_errors);
    RUN(arguments_files_and_memory);
    return TEST_DONE();
}
