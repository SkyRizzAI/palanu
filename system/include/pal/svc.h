/* PalanuOS service supervisor. A service is a background unit: either a built-in
 * one (C code registered at boot with a start and a stop function) or an installed kind=service app (a process the
 * supervisor spawns, with its stdout and stderr in a per-service log).
 *
 * Boot starts the enabled services in `after` order (psys_svc_start_all). Each service is in one state: STOPPED,
 * WAITING_MEMORY (admission refused, retried every 5 s), STARTING, RUNNING, or FAILED (five failures within ten minutes;
 * a manual start resets it). Restart policy: never, on-failure, or always, with a backoff of 1 s doubling to 60 s.
 *
 * Enabled/disabled is kept in /data/services ("id=enabled" or "id=disabled"). start and stop record the choice too, so a
 * service stopped by hand stays stopped after a restart. Radio services keep their choice in the radio settings instead
 * (psys_svc_builtin_t.on_enabled), because docs/RADIO.md defines their boot policy. */
#ifndef PAL_SVC_H
#define PAL_SVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/nema.h"

#define PSYS_SVC_MAX 16          /* services known at once: built-in and installed */
#define PSYS_SVC_ID_LEN 24
#define PSYS_SVC_NAME_LEN 32
#define PSYS_SVC_REASON_LEN 96
#define PSYS_SVC_LOG_BYTES (8u * 1024u) /* per service, in PSRAM, allocated at its first start */
#define PSYS_SVC_RETRY_MS 5000u         /* WAITING_MEMORY retry */
#define PSYS_SVC_FAIL_MAX 5             /* failures within the window before FAILED */
#define PSYS_SVC_FAIL_WINDOW_MS (10u * 60u * 1000u)
#define PSYS_SVC_BACKOFF_MS 1000u       /* first restart delay; doubles, capped at the maximum */
#define PSYS_SVC_BACKOFF_MAX_MS 60000u

typedef enum {
    PSYS_SVC_RESTART_NEVER = 0,
    PSYS_SVC_RESTART_ON_FAILURE,
    PSYS_SVC_RESTART_ALWAYS,
} psys_svc_restart_t;

typedef enum {
    PSYS_SVC_STOPPED = 0,
    PSYS_SVC_WAITING_MEMORY,
    PSYS_SVC_STARTING,
    PSYS_SVC_RUNNING,
    PSYS_SVC_FAILED,
} psys_svc_state_t;

/* A built-in service. The table entry is copied at registration; the functions run in the supervisor's context (never
 * from an interrupt) and may block for a second or two, like the radio starts. */
typedef struct {
    const char *id;   /* unique; [a-z0-9-] */
    const char *name; /* shown in listings; NULL = id */
    const char *const *after; /* ids that start before this one (ordering only); NULL-terminated, or NULL for none */
    psys_svc_restart_t restart;
    bool autostart;   /* enabled at boot unless the saved choice says otherwise */
    bool core;        /* started even in safe mode, and cannot be stopped */
    nema_err_t (*start)(void); /* NEMA_ERR_NOMEM = waiting for memory; NULL = already running (started by boot) */
    void (*stop)(void);        /* NULL when the service has no stop */
    bool (*gate)(void);        /* NULL: no condition. Otherwise the service runs only while this returns true */
    void (*on_enabled)(bool enabled); /* NULL: the choice is kept in /data/services. Otherwise the owner keeps it */
    const char *waiting_for; /* shown while the gate is closed, e.g. "a Wi-Fi address"; NULL for a generic text */
} psys_svc_builtin_t;

/* Register a built-in service. Before psys_svc_start_all(). NEMA_ERR_FULL when the table is full, NEMA_ERR_EXIST for a
 * duplicate id. */
nema_err_t psys_svc_register_builtin(const psys_svc_builtin_t *b);

/* Boot: reads /data/services, starts the enabled services in `after` order (core ones even in safe mode), and starts the
 * supervisor task. A cycle in `after` marks the services in it FAILED and starts the rest. */
void psys_svc_start_all(bool safe_mode);
bool psys_svc_safe_mode(void);

/* Wake the supervisor to re-check the gates (Wi-Fi address changed). Safe from any task; never blocks. */
void psys_svc_poke(void);

/* Manual control: the shell's svc command and the API. Start and stop record the choice (see above). */
nema_err_t psys_svc_start(const char *id);
nema_err_t psys_svc_stop(const char *id);
nema_err_t psys_svc_restart(const char *id);
nema_err_t psys_svc_set_enabled(const char *id, bool on); /* records the choice; does not start or stop */

/* Called by an installed service when it has finished starting (pal_svc_ready). NEMA_ERR_NOENT when the caller is not a
 * service started by the supervisor. */
nema_err_t psys_svc_ready_self(void);

typedef struct {
    char id[PSYS_SVC_ID_LEN];
    char name[PSYS_SVC_NAME_LEN];
    psys_svc_state_t state;
    bool enabled;
    bool core;
    bool installed;     /* a package in the registry, not built into the firmware */
    nema_pid_t pid;     /* 0 when no process */
    uint32_t restarts;  /* restarts since boot (not counting the first start) */
    uint32_t failures;  /* failures within the window */
    uint64_t started_us; /* when the current run started */
    uint64_t log_total;  /* bytes ever written to the log */
    char reason[PSYS_SVC_REASON_LEN]; /* why it is in this state, or "" */
} psys_svc_info_t;

size_t psys_svc_count(void);
bool psys_svc_info(size_t i, psys_svc_info_t *out);
bool psys_svc_find(const char *id, psys_svc_info_t *out);
const char *psys_svc_state_name(psys_svc_state_t s);
const char *psys_svc_restart_name(psys_svc_restart_t r);
/* Told about every change of a service's state (the system console prints the boot's status lines from it). It runs in the
 * supervisor's context with the table locked: it must only copy what it needs. NULL removes it. */
typedef void (*psys_svc_status_sink_t)(const char *id, const char *name, psys_svc_state_t was, psys_svc_state_t now,
                                       const char *why);
void psys_svc_set_status_sink(psys_svc_status_sink_t sink);

/* The log: copies the bytes after *cursor (initially 0 reads what is kept). The cursor moves past what was copied; bytes
 * that were overwritten before the reader came are skipped. Returns the bytes copied, 0 when there is nothing new, or
 * NEMA_ERR_NOENT for an unknown service. */
int psys_svc_log_read(const char *id, uint64_t *cursor, char *buf, size_t cap);
/* Waits until the log holds bytes after cursor (or timeout, or a fatal signal to the caller). NEMA_OK when there are
 * new bytes, NEMA_ERR_TIMEOUT, or NEMA_ERR_INTR. */
nema_err_t psys_svc_log_wait(const char *id, uint64_t cursor, int32_t timeout_ms);

/* Boot health: true once every enabled autostart service is RUNNING, FAILED, or WAITING_MEMORY (the outcomes that
 * need no further boot work). Feeds the boot-attempt counter (pal/bootcount.h). */
bool psys_svc_settled(void);

/* Test seams for the host unit tests (the radio manager has the same kind of seams). */
void psys_svc_set_clock(uint64_t (*now_us)(void));
/* Makes psys_svc_start_all skip the supervisor task; the test then calls psys_svc_step with its own clock. */
void psys_svc_test_no_task(void);
/* One pass of the supervisor at time now_us: reaps, retries, gates, timers. Returns the next deadline, 0 if none. */
uint64_t psys_svc_step(uint64_t now_us);
/* Reset the table to empty (tests only; the firmware boots once). */
void psys_svc_test_reset(void);

/* The smallest spare stack the supervisor task has had since boot, in bytes (a budget check: keep it above 1 KB). */
size_t psys_svc_stack_spare(void);

#endif
