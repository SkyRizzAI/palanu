/* Nema processes.
 *
 * A process is one running app: an id, arguments, a handle table (0, 1, 2 are stdio), pending signals,
 * a working directory, and an exit code. Shell, GUI app, service, and SSH client are all the same thing;
 * they differ only in what is plugged into their handles.
 *
 * Built-in apps are cooperative: a signal makes blocking calls return NEMA_ERR_INTR and the app is
 * expected to return. There is no memory protection between built-in apps (the chip has no MMU);
 * isolation for third-party apps comes from the sandboxed runtime, not from here.
 */
#ifndef NEMA_PROC_H
#define NEMA_PROC_H

#include "nema/obj.h"
#include "nema/task.h"

typedef int16_t nema_pid_t;
#define NEMA_PROC_MAX 32 /* one owner id per slot, see mem.h */
#define NEMA_HANDLES_MAX 16
#define NEMA_CWD_MAX 64
#define NEMA_PROC_NAME_MAX 16

/* Signals. INT/TERM/KILL/HUP are fatal to blocking calls; WINCH is informational.
 * TERM is the polite stop: blocking calls return NEMA_ERR_INTR and the app is expected to return. If it is still
 * running NEMA_TERM_GRACE_MS later, it is killed. KILL poisons the process: every blocking call returns
 * NEMA_ERR_CANCELED at once, and its wasm instance (if any) is terminated. Nothing is interrupted while the process
 * is inside nema_cancel_defer(); the signal applies when the outermost nema_cancel_allow() returns. */
#define NEMA_SIG_INT 0x01u
#define NEMA_SIG_TERM 0x02u
#define NEMA_SIG_KILL 0x04u
#define NEMA_SIG_HUP 0x08u
#define NEMA_SIG_WINCH 0x10u
#define NEMA_SIG_FATAL (NEMA_SIG_INT | NEMA_SIG_TERM | NEMA_SIG_KILL | NEMA_SIG_HUP)
#define NEMA_TERM_GRACE_MS 3000

typedef enum { NEMA_PROC_FREE = 0, NEMA_PROC_RUNNING, NEMA_PROC_ZOMBIE } nema_proc_state_t;

typedef struct nema_proc {
    nema_pid_t pid;
    nema_proc_state_t state;
    char name[NEMA_PROC_NAME_MAX];
    nema_owner_t owner;
    nema_pid_t parent;
    nema_pid_t pgid; /* process group: a pipeline is one group, so Ctrl-C reaches all of it */
    int exit_code;
    nema_obj_t *fd[NEMA_HANDLES_MAX];
    volatile uint32_t sigpending;
    nema_obj_t sig_obj;  /* notified when a signal is raised */
    nema_obj_t exit_obj; /* notified when the process becomes a zombie */
    bool killed;         /* KILL applied: blocking calls return NEMA_ERR_CANCELED */
    bool term_pending;   /* TERM sent: killed at term_at_us unless it exits first */
    uint8_t defer;       /* nema_cancel_defer() nesting: signals wait until it returns to zero */
    void *exit_jmp;          /* where nema_cancel_strike() jumps to end the process through the normal exit path */
    uint16_t cancel_strikes; /* blocking calls made after the KILL: past NEMA_CANCEL_STRIKES the kernel ends the process */
    uint64_t term_at_us;
    void (*kill_hook)(void *ctx); /* run at KILL, under the process table lock (wasm: terminate the instance) */
    void *kill_ctx;
    char cwd[NEMA_CWD_MAX];
    int argc;
    char **argv;
    const struct nema_app *app;
    uint64_t start_us;
    uint32_t flags;
} nema_proc_t;

/* An app that the kernel can launch by name. */
typedef struct nema_app {
    const char *name;
    const char *help; /* one line, for `help` and listings */
    int (*main)(int argc, char **argv);
    size_t stack; /* bytes; 0 = default */
    uint32_t flags;
} nema_app_t;

#define NEMA_APP_STACK_DEFAULT (8u * 1024u)
#define NEMA_APP_HIDDEN 0x1u /* not listed in /bin */

/* The registry grows as needed (kernel memory); there is no fixed limit. Names are unique. */
nema_err_t nema_app_register(const nema_app_t *app);
/* Removes a command. NEMA_ERR_BUSY while a process runs it, NEMA_ERR_NOENT if unknown. */
nema_err_t nema_app_unregister(const char *name);
const nema_app_t *nema_app_find(const char *name);
size_t nema_app_count(void);
const nema_app_t *nema_app_at(size_t index);

#define NEMA_SPAWN_DETACH 0x1u /* reap automatically when it exits */

/* Per-process heap quotas, applied to the process owner's allocations. A process that would go past its quota gets
 * NULL from nema_alloc (the same as any refused allocation); other processes and the system are not affected. */
#define NEMA_PROC_QUOTA_EXT_DEFAULT (2u * 1024u * 1024u) /* PSRAM bytes */
#define NEMA_PROC_QUOTA_INT_DEFAULT (64u * 1024u)        /* internal RAM bytes (heap only, not the stack) */

typedef struct {
    const char *name; /* registered app name */
    int argc;
    const char *const *argv; /* argv[0] defaults to name when argc == 0 */
    nema_obj_t *stdio[3];    /* NULL: inherit from the calling process, or the null device */
    const char *cwd;         /* NULL: inherit */
    nema_pid_t pgid;         /* 0: a group of its own; otherwise join this group */
    uint32_t flags;
    size_t mem_quota_ext; /* PSRAM bytes; 0 = NEMA_PROC_QUOTA_EXT_DEFAULT */
    size_t mem_quota_int; /* internal heap bytes; 0 = NEMA_PROC_QUOTA_INT_DEFAULT */
} nema_spawn_t;

nema_err_t nema_proc_spawn(const nema_spawn_t *spec, nema_pid_t *pid);
/* Wait for exit and reap. Returns NEMA_OK with *code set, NEMA_ERR_TIMEOUT, or NEMA_ERR_INTR. */
nema_err_t nema_proc_wait(nema_pid_t pid, int *code, int32_t timeout_ms);
/* Nobody will wait for pid: it is reaped by itself when it exits (or at once, if it already has). For a parent that
 * ends before its children. NEMA_ERR_NOENT if pid is not a live process. */
nema_err_t nema_proc_detach(nema_pid_t pid);
nema_err_t nema_proc_signal(nema_pid_t pid, uint32_t sig);
int nema_proc_signal_group(nema_pid_t pgid, uint32_t sig);
/* Signals a process and every process descended from it, whatever their groups (background jobs have a group of
 * their own): this is how closing a session ends everything that session started. Returns how many were signalled. */
int nema_proc_signal_tree(nema_pid_t root, uint32_t sig); /* returns how many processes were signalled */
nema_proc_t *nema_proc_get(nema_pid_t pid);
/* The object notified when pid exits (becomes a zombie), for a waiter that watches several processes at once. Borrowed:
 * valid until the process is reaped. NULL when pid is not a live process. */
nema_obj_t *nema_proc_exit_obj(nema_pid_t pid);
size_t nema_proc_list(nema_proc_t *out, size_t max); /* copies of live process records, without handles */

/* The calling process, or NULL in a system task. */
nema_proc_t *nema_self(void);
bool nema_interrupted(void); /* a fatal signal is pending for the caller (not while deferred) */
uint32_t nema_sig_take(uint32_t mask); /* clear and return the pending bits in mask */
/* A killed process that keeps calling blocking functions ignores the cancellation (it would spin and starve every other
 * task). After this many such calls the kernel ends it at the call boundary, on its own task, holding no lock. */
#define NEMA_CANCEL_STRIKES 64
void nema_cancel_strike(void); /* called where a blocking call is about to return CANCELED; may not return */
bool nema_canceled(void);    /* the caller was killed (not while deferred): blocking calls return CANCELED */

/* Defer signals for the caller until the matching allow (nests). Used around a file write so it is never cut short. */
void nema_cancel_defer(void);
void nema_cancel_allow(void);

/* Registers what the caller needs done when it is killed (the caller's own WAMR instance). fn(ctx) runs once at
 * KILL, or at once if already killed. Pass fn NULL to remove it before the resource goes away. */
nema_err_t nema_proc_set_kill_hook(void (*fn)(void *ctx), void *ctx);

/* ---- handle API (the calling process's handle table) ---- */
int nema_read(int fd, void *buf, size_t n);                           /* blocks */
int nema_read_timeout(int fd, void *buf, size_t n, int32_t timeout_ms);
int nema_write(int fd, const void *buf, size_t n);                    /* blocks until all written */
int nema_close(int fd);
int nema_dup(int fd);
nema_err_t nema_dup2(int oldfd, int newfd);
int nema_handle_install(nema_obj_t *o);        /* takes a new reference; returns the handle or <0 */
nema_obj_t *nema_handle_obj(int fd);           /* borrowed pointer, valid while the handle is open */
nema_err_t nema_ioctl(int fd, uint32_t req, void *arg);

typedef struct {
    int fd;
    uint32_t events;
    uint32_t revents;
} nema_pollfd_t;
int nema_poll(nema_pollfd_t *fds, size_t n, int32_t timeout_ms);

void nema_proc_init(void);

#endif
