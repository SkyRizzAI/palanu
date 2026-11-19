/* Runs a .papp's WebAssembly module in the calling Nema process (WAMR fast interpreter).
 *
 * The app is a process like any other: its stdin, stdout and stderr are the process handles, so a pipe, a
 * redirect, Ctrl-C and `ps` all work on it unchanged. Everything an app can do goes through the `palanu` import
 * module. Its table is generated from the api/palanu headers (api_wasm_glue.c): every import checks the capability
 * its definition names, then calls the implementation in the system/api sources. This file owns the runtime, the
 * allocator, and the per-app state those implementations read through api_ctx.h. */
#include <stdio.h>
#include <string.h>

#include "api_wasm.h"
#include "pal/papp.h"
#include "pal/perms.h"
#include "wasm_export.h"

#if !defined(WASM_MEM_ALLOC_WITH_USAGE) || WASM_MEM_ALLOC_WITH_USAGE != 1
#error "WAMR must be built with WASM_MEM_ALLOC_WITH_USAGE=1: the allocator hook is charged per process (docs/adr/0003)"
#endif

#define OPEN_MAX_APP 16 /* handles an app holds at once: files, directories, sockets (api level 2) */
#define LINEAR_PAGE 65536u
/* The instantiation's own exec env is only made when the module has a start function; the run gets the manifest stack
 * (create_exec_env), so the stack is not allocated twice at its full size. */
#define INSTANTIATE_STACK 4096u

struct api_ctx {
    const pal_papp_t *app;
    int argc;
    char **argv;
    bool exited;
    int exit_code;
    const char *stop; /* why the app must end at its next return to the runtime ("exit", "interrupted"), or NULL */
    int32_t fds[OPEN_MAX_APP]; /* handles this app opened itself; -1 when unused */
    char tmp[64];             /* the app's private /tmp directory for this run: /tmp/.app/<id>-<pid> */
    api_ext_t ext;            /* per-module state (api_ctx.h): events, child processes, terminal mode */
};

static nema_owner_t s_owner;  /* "wasm": the runtime's own one-time state */
static bool s_starting;       /* true while the runtime's one-time init runs: its bytes belong to s_owner */

/* ---- allocator: WAMR has one global hook, so the owner is looked up per call ----
 * WAMR allocates from the task that runs the call. For a process that is the process: its module image, instance,
 * exec env stack, and linear memory are charged to it (quota, sweep on exit). Outside a process (boot, tests) the
 * bytes go to "wasm". Blocks prefer PSRAM. Linear memory is zeroed, as WebAssembly requires for grown pages. */
static nema_owner_t owner_now(void) {
    if (s_starting) return s_owner;
    nema_proc_t *p = nema_self();
    return p ? p->owner : s_owner;
}

static void *w_malloc(mem_alloc_usage_t usage, unsigned size) {
    uint32_t flags = usage == Alloc_For_LinearMemory ? NEMA_ALLOC_ZERO : 0;
    return nema_alloc_ex(NEMA_MEM_ANY, owner_now(), size ? size : 1, 0, flags);
}

static void *w_realloc(mem_alloc_usage_t usage, bool mapped, void *p, unsigned size) {
    (void)mapped;
    if (!p) return w_malloc(usage, size);
    size_t old = nema_alloc_size(p);
    if (size <= old) return p;
    void *n = w_malloc(usage, size); /* zeroed when linear memory: the grown pages start empty */
    if (!n) return NULL;             /* the old block stays valid, so a refused growth changes nothing */
    memcpy(n, p, old);
    nema_free(p);
    return n;
}

static void w_free(mem_alloc_usage_t usage, void *p) {
    (void)usage;
    nema_free(p);
}

/* ---- state accessors (api_ctx.h): the implementations see the app only through these ---- */

int api_argc(const api_ctx_t *c) { return c->argc; }

const char *api_argv(const api_ctx_t *c, int i) { return i < 0 || i >= c->argc ? NULL : c->argv[i]; }

bool api_fd_owned(const api_ctx_t *c, int32_t fd) {
    if (fd <= 2) return false;
    for (int i = 0; i < OPEN_MAX_APP; i++)
        if (c->fds[i] == fd) return true;
    return false;
}

bool api_fd_has_slot(const api_ctx_t *c) {
    for (int i = 0; i < OPEN_MAX_APP; i++)
        if (c->fds[i] < 0) return true;
    return false;
}

bool api_fd_add(api_ctx_t *c, int32_t fd) {
    for (int i = 0; i < OPEN_MAX_APP; i++) {
        if (c->fds[i] >= 0) continue;
        c->fds[i] = fd;
        return true;
    }
    return false;
}

bool api_fd_remove(api_ctx_t *c, int32_t fd) {
    if (!api_fd_owned(c, fd)) return false;
    for (int i = 0; i < OPEN_MAX_APP; i++)
        if (c->fds[i] == fd) c->fds[i] = -1;
    return true;
}

void api_exit(api_ctx_t *c, int32_t code) {
    c->exited = true;
    c->exit_code = code;
    c->stop = "exit";
}

void api_stop(api_ctx_t *c, const char *reason) {
    if (!c->stop) c->stop = reason;
}

/* ---- hooks the generated glue calls (api_wasm.h) ---- */

api_ctx_t *api_ctx_of(wasm_exec_env_t env) {
    return wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(env));
}

/* The manifest's permission bit for a capability name (system/include/pal/papp.h). Unknown names have none. */
static uint32_t perm_bit(const char *cap) {
    if (strcmp(cap, "fs") == 0) return PAL_PERM_FS;
    if (strcmp(cap, "fs.home") == 0) return PAL_PERM_FS_HOME;
    if (strcmp(cap, "fs.media") == 0) return PAL_PERM_FS_MEDIA;
    if (strcmp(cap, "net") == 0) return PAL_PERM_NET;
    if (strcmp(cap, "net.listen") == 0) return PAL_PERM_NET_LISTEN;
    if (strcmp(cap, "proc.spawn") == 0) return PAL_PERM_PROC;
    if (strcmp(cap, "crypto.keys") == 0) return PAL_PERM_CRYPTO_KEYS;
    if (strcmp(cap, "settings.read") == 0) return PAL_PERM_SETTINGS_READ;
    if (strcmp(cap, "settings.write") == 0) return PAL_PERM_SETTINGS_WRITE;
    if (strcmp(cap, "notify") == 0) return PAL_PERM_NOTIFY;
    if (strcmp(cap, "dev") == 0) return PAL_PERM_DEV;
    return 0;
}

bool api_perm(const api_ctx_t *c, const char *cap) {
    uint32_t bit = perm_bit(cap);
    if (bit == 0 || (c->app->perms & bit) == 0) return false;
    if (!(bit & PAL_PERM_ASKED)) return true;
    api_ctx_t *m = (api_ctx_t *)c; /* the answer is cached in the run's context: asked once, read on every call after */
    if (m->ext.perm_ok & bit) return true;
    if (m->ext.perm_no & bit) return false;
    bool ok = psys_perm_check(c->app->id, c->app->name[0] ? c->app->name : c->app->id, bit);
    if (ok) m->ext.perm_ok |= bit;
    else m->ext.perm_no |= bit;
    return ok;
}

bool api_perm_quiet(const api_ctx_t *c, const char *cap) {
    uint32_t bit = perm_bit(cap);
    if (bit == 0 || (c->app->perms & bit) == 0) return false;
    return !(bit & PAL_PERM_ASKED) || psys_perm_state(c->app->id, bit) != PAL_PERM_DENIED;
}

bool api_cap_ok(wasm_exec_env_t env, const char *cap) { return api_perm(api_ctx_of(env), cap); }

const char *api_app_id(const api_ctx_t *c) { return c->app->id; }

const char *api_tmp_dir(const api_ctx_t *c) { return c->tmp; }

api_ext_t *api_ext(api_ctx_t *c) { return &c->ext; }

/* A context for one run. Its memory is charged like the rest of the run (the calling process, or "wasm"). */
api_ctx_t *api_ctx_new(const pal_papp_t *app, int argc, char **argv) {
    struct api_ctx *c = (struct api_ctx *)nema_alloc(NEMA_MEM_ANY, owner_now(), sizeof(*c));
    if (!c) return NULL;
    c->app = app;
    c->argc = argc;
    c->argv = argv;
    c->exited = false;
    c->exit_code = 0;
    c->stop = NULL;
    for (int i = 0; i < OPEN_MAX_APP; i++) c->fds[i] = -1;
    c->ext.ev_fd = -1;
    c->ext.crypto = NULL;
    c->ext.perm_ok = c->ext.perm_no = 0;
    c->ext.tty_fd = -1;
    c->ext.tty_saved = false;
    c->ext.legend_fd = -1;
    for (int i = 0; i < API_KIDS_MAX; i++) c->ext.kids[i] = 0;
    nema_proc_t *p = nema_self();
    snprintf(c->tmp, sizeof(c->tmp), "/tmp/.app/%s-%d", app->id, p ? (int)p->pid : 0);
    return c;
}

/* Ends the app's use of the terminal (its mode goes back), reaps the children it left running, closes what it left open,
 * removes its private /tmp, and frees the context. */
void api_ctx_free(api_ctx_t *c) {
    if (!c) return;
    api_tty_restore(c);
    api_legend_release(c);
    api_ui_release(c);
    api_kids_release(c);
    for (int i = 0; i < OPEN_MAX_APP; i++)
        if (c->fds[i] >= 0) nema_close(c->fds[i]);
    api_fs_release(c);
    nema_free(c);
}

/* A stop request from the implementation: the instance stops at this return, with the reason as its exception. */
void api_after(wasm_exec_env_t env, api_ctx_t *c) {
    if (c->stop) wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), c->stop);
}

static bool runtime_start(void);
static bool runtime_init(void) {
    static int state; /* 0 untouched, 1 starting, 2 ready, 3 failed */
    int expect = 0;
    if (__atomic_compare_exchange_n(&state, &expect, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        bool ok = runtime_start();
        __atomic_store_n(&state, ok ? 2 : 3, __ATOMIC_RELEASE);
        return ok;
    }
    while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 1) nema_sleep_ms(5);
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2;
}

static bool runtime_start(void) {
    s_owner = nema_owner_register("wasm");
    RuntimeInitArgs a;
    memset(&a, 0, sizeof(a));
    a.mem_alloc_type = Alloc_With_Allocator;
    a.mem_alloc_option.allocator.malloc_func = (void *)w_malloc;
    a.mem_alloc_option.allocator.realloc_func = (void *)w_realloc;
    a.mem_alloc_option.allocator.free_func = (void *)w_free;
    s_starting = true;
    bool ok = wasm_runtime_full_init(&a) &&
              wasm_runtime_register_natives("palanu", api_palanu_natives, api_palanu_natives_count);
    s_starting = false;
    return ok;
}

static void say(const char *fmt, const char *a, const char *b) {
    char m[160];
    int n = snprintf(m, sizeof(m), fmt, a, b);
    nema_write(2, m, (size_t)n);
}

static int run_app(const pal_papp_t *app, uint8_t *image, int argc, char **argv);

int pal_wasm_run(const pal_papp_t *app, uint8_t *image, int argc, char **argv) {
    if (!runtime_init()) {
        say("%s: the WebAssembly runtime did not start%s\n", app->id, "");
        nema_free(image);
        return 126;
    }
    if (!wasm_runtime_init_thread_env()) {
        say("%s: the WebAssembly runtime cannot run on this task%s\n", app->id, "");
        nema_free(image);
        return 126;
    }
    int code = run_app(app, image, argc, argv);
    wasm_runtime_destroy_thread_env();
    return code;
}

/* KILL: the instance is terminated by the killer (WAMR sets the exception) and the exec env gets a kill request, which
 * the interpreter checks where it counts instructions (a local WAMR patch, third_party/README.md): even a loop that
 * never calls an import stops. */
typedef struct {
    wasm_module_inst_t inst;
    wasm_exec_env_t env; /* NULL until the run starts */
} kill_ctx_t;

static void kill_instance(void *ctx) {
    kill_ctx_t *k = (kill_ctx_t *)ctx;
    wasm_runtime_terminate(k->inst); /* imports and blocking calls end at once */
    if (k->env) wasm_runtime_request_kill(k->env); /* a loop with no imports stops at its next metering point */
}

static int run_app(const pal_papp_t *app, uint8_t *image, int argc, char **argv) {
    char err[96] = "";
    /* One copy only: the module moves to the start of the image (the loader reads it at aligned addresses) and the
     * image is freed as soon as the loader has copied what it keeps (wasm_binary_freeable). */
    memmove(image, app->payload, app->payload_len);
    LoadArgs la;
    memset(&la, 0, sizeof(la));
    la.wasm_binary_freeable = true;
    wasm_module_t mod = wasm_runtime_load_ex(image, (uint32_t)app->payload_len, &la, err, sizeof(err));
    nema_free(image);
    int status = 126;
    api_ctx_t *ctx = NULL;
    if (!mod) {
        say("%s: cannot load: %s\n", app->id, err);
        return 126;
    }
    uint32_t stack = (app->stack_kb ? app->stack_kb : 16u) * 1024u;
    uint32_t limit_kb = app->mem_kb ? app->mem_kb : 256u;
    /* memory.grow is capped at the manifest's mem: whole 64 KB pages, at least one. A module whose initial memory is
     * already above the cap is refused below, by the check after instantiation. */
    InstantiationArgs ia;
    memset(&ia, 0, sizeof(ia));
    ia.default_stack_size = INSTANTIATE_STACK;
    uint64_t pages = (uint64_t)limit_kb * 1024u / LINEAR_PAGE;
    ia.max_memory_pages = pages ? (uint32_t)pages : 1u;
    wasm_module_inst_t inst = wasm_runtime_instantiate_ex(mod, &ia, err, sizeof(err));
    if (!inst) {
        say("%s: cannot start: %s\n", app->id, err);
        goto unload;
    }
    wasm_memory_inst_t mem = wasm_runtime_get_default_memory(inst);
    if (mem && (uint64_t)wasm_memory_get_cur_page_count(mem) * wasm_memory_get_bytes_per_page(mem) > (uint64_t)limit_kb * 1024u) {
        say("%s: needs more memory than its manifest allows%s\n", app->id, "");
        goto uninst;
    }
    ctx = api_ctx_new(app, argc, argv);
    if (!ctx) {
        say("%s: out of memory%s\n", app->id, "");
        goto uninst;
    }
    wasm_runtime_set_custom_data(inst, ctx);
    kill_ctx_t kctx = {.inst = inst, .env = NULL};
    nema_proc_set_kill_hook(kill_instance, &kctx);
    wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "_start");
    if (!fn) fn = wasm_runtime_lookup_function(inst, "main");
    if (!fn) {
        say("%s: the module exports no _start%s\n", app->id, "");
        goto uninst;
    }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, stack);
    if (!env) {
        say("%s: out of memory%s\n", app->id, "");
        goto uninst;
    }
    kctx.env = env;
#if WASM_ENABLE_INSTRUCTION_METERING != 0
    if (app->fuel_m) wasm_runtime_set_instruction_count_limit(env, (int)(app->fuel_m * 1000000u));
#endif
    uint32_t argv_cells[2] = {0, 0};
    bool ok = wasm_runtime_call_wasm(env, fn, wasm_func_get_param_count(fn, inst) ? 2 : 0, argv_cells);
    if (ctx->exited) {
        status = ctx->exit_code;
    } else if (ok) {
        status = 0;
    } else {
        const char *ex = wasm_runtime_get_exception(inst);
        say("%s: stopped: %s\n", app->id, ex ? ex : "trap");
        status = strcmp(ex ? ex : "", "Exception: interrupted") == 0 ? 130 : 139;
    }
    nema_proc_set_kill_hook(NULL, NULL); /* the exec env is about to go */
    wasm_runtime_destroy_exec_env(env);
uninst:
    nema_proc_set_kill_hook(NULL, NULL); /* before the instance can go: a kill must not reach a freed instance */
    wasm_runtime_deinstantiate(inst);
    api_ctx_free(ctx); /* NULL when the run never got its context */
unload:
    wasm_runtime_unload(mod);
    return status;
}
