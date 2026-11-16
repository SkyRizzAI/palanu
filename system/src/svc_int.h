/* Internal to the service supervisor (system/src/svc*.c): the table, the global state, and the helpers the files share.
 * Nothing outside system/src/svc*.c may include this file. */
#ifndef SYSTEM_SVC_INT_H
#define SYSTEM_SVC_INT_H

#include <stdbool.h>
#include <stdint.h>

#include "nema/nema.h"
#include "pal/svc.h"
#include "pal/registry.h"

#define STORE_PATH "/data/services"
#define STORE_MAX 512u
#define SUPERVISOR_STACK 6144u /* LittleFS writes (the saved choices) and the settings file need more than 3 KB: found on the board */
#define SUPERVISOR_TICK_MS 1000u
#define READY_TIMEOUT_MS 5000u
#define RETRY_MS PSYS_SVC_RETRY_MS
#define PIPE_BYTES 1024u
#define DRAIN_CHUNK 128u
#define LINE_MAX 160u
#define EX_TEMPFAIL 75 /* the .papp runner's "refused for now" (papp_pkg.c) */
#define STOP_KILL_WAIT_MS 2000u
#define REQ_TIMEOUT_MS 30000u
#define WAITSET_MAX NEMA_WAIT_OBJS_MAX

enum { OVR_NONE = 0, OVR_ON, OVR_OFF };
enum { DUE_NONE = 0, DUE_RESTART, DUE_RETRY };
enum { REQ_NONE = 0, REQ_START, REQ_STOP, REQ_RESTART };

typedef struct {
    bool used;
    bool installed; /* a package in the registry (kind service); its definition is the registry entry */
    psys_svc_builtin_t bi;
    char id[PSYS_SVC_ID_LEN];
    char name[PSYS_SVC_NAME_LEN];
    psys_svc_restart_t restart;
    bool enabled;
    psys_svc_state_t state;
    char reason[PSYS_SVC_REASON_LEN];
    bool stop_req;      /* the stop was asked for (by hand or by a gate): the exit is not a failure */
    volatile bool ready_req; /* the process called pal_svc_ready */
    uint64_t started_us;
    uint64_t due_us;     /* next action (restart, memory retry); 0 = none */
    uint8_t due_kind;    /* DUE_* */
    uint64_t ready_due_us; /* STARTING installed: RUNNING at this time without a ready call; 0 = none */
    uint32_t restarts;
    uint64_t fail_us[PSYS_SVC_FAIL_MAX]; /* the last failures, oldest first */
    uint8_t nfail;
    uint32_t failures; /* failures within the window, as of the last failure */
    nema_pid_t pid;    /* 0 when no process */
    nema_proc_t *proc;
    nema_obj_t *exit_obj;
    nema_obj_t *rd;    /* read end of the process's stdout and stderr pipe */
    bool rd_eof;
    uint8_t *log;      /* PSYS_SVC_LOG_BYTES ring, allocated at the first start */
    volatile uint64_t log_total;
    nema_obj_t log_obj; /* notified when log_total grows: svc logs -f waits on it */
    uint16_t linelen;
    char line[LINE_MAX];
} svc_t;

typedef struct {
    bool used;
    char id[PSYS_SVC_ID_LEN];
    uint8_t ovr;
} saved_t;

typedef struct {
    nema_port_mutex_t *lock;     /* every table field below, except the volatile ones */
    nema_port_mutex_t *req_lock; /* one shell request at a time */
    svc_t t[PSYS_SVC_MAX];
    saved_t saved[PSYS_SVC_MAX]; /* the choices read from /data/services, plus the ones made since */
    bool store_loaded, safe, started, no_task, task_on;
    volatile bool kick;
    volatile int req_op;
    char req_id[PSYS_SVC_ID_LEN];
    nema_err_t req_result;
    volatile bool req_done;
    uint64_t (*clock)(void);
    nema_obj_t control; /* notified to wake the supervisor */
    nema_obj_t reply;   /* notified when a request is answered */
    nema_owner_t owner;
} svc_global_t;

/* The whole table is one block from the heap, PSRAM when there is some (NEMA_MEM_ANY): about 10 KB of structures that
 * would otherwise sit in internal RAM. Allocated by svc_ensure_init; NULL before. */
extern svc_global_t *svc_g;


/* svc.c: the table, locks, the installed-service sync, the supervisor, and the control API. */
void svc_lock(void);
void svc_unlock(void);
uint64_t svc_now_us(void);
void svc_setstr(char *dst, size_t cap, const char *src);
bool svc_valid_id(const char *id);
void svc_set_state(svc_t *e, psys_svc_state_t s, const char *why);
svc_t *svc_find(const char *id);
svc_t *svc_add(void);
svc_t *svc_find_or_ensure(const char *id);
svc_t *svc_ensure_installed(const char *id);
void svc_sync_installed(void);
bool svc_visible(const svc_t *e);
void svc_ensure_init(void);
bool svc_proc_gone(const svc_t *e);
bool svc_pred_gone(void *ctx);
bool svc_wait_gone(svc_t *e, uint32_t ms);

/* svc_store.c: /data/services and the choice of each service. */
uint8_t svc_saved_ovr(const char *id);
void svc_store_load(void);
void svc_store_save(void);
void svc_record_choice(svc_t *e, bool on);
void svc_apply_saved(svc_t *e);

/* svc_log.c: the per-service log ring and its line feed to the nema log. */
void svc_ensure_log(svc_t *e);
void svc_log_bytes(svc_t *e, const char *d, size_t n);
void svc_drain(svc_t *e);
void svc_close_rd(svc_t *e);

/* svc_query.c: read-only views. */
void svc_fill(const svc_t *e, psys_svc_info_t *out);

#endif
