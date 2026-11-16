/* The state of one WebAssembly app, as the API implementations (the system/api sources) see it. The struct is
 * private to wasm_rt.c; these accessors are the only way in, so the implementations need no WAMR header. */
#ifndef SYSTEM_API_CTX_H
#define SYSTEM_API_CTX_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/papp.h"

typedef struct api_ctx api_ctx_t;

int api_argc(const api_ctx_t *c);
const char *api_argv(const api_ctx_t *c, int i); /* NULL when i is out of range */
bool api_fd_owned(const api_ctx_t *c, int32_t fd); /* opened by the app itself; 0 to 2 are never owned */
bool api_fd_has_slot(const api_ctx_t *c);
bool api_fd_add(api_ctx_t *c, int32_t fd);
bool api_fd_remove(api_ctx_t *c, int32_t fd); /* true if fd was the app's own; forgotten, the caller closes it */
void api_exit(api_ctx_t *c, int32_t code);
void api_stop(api_ctx_t *c, const char *reason); /* the app ends at its next return to the runtime */

/* The manifest grants the capability: "fs", "fs.home", "fs.media", "net", "net.listen", "crypto.keys", "settings.read",
 * "settings.write", "notify", "dev". Unknown names are refused. */
bool api_perm(const api_ctx_t *c, const char *cap);
const char *api_app_id(const api_ctx_t *c);
const char *api_tmp_dir(const api_ctx_t *c); /* the app's private /tmp directory for this run (absolute path) */
/* Ends the app's use of the filesystem: removes its private /tmp (system/api/fs.c). Called once, when the app ends. */
void api_fs_release(api_ctx_t *c);

/* A new context for one run of app (the runtime makes one per run; tests make one inside a process). Free it with
 * api_ctx_free when the run is over: that closes the app's handles and removes its private /tmp. */
api_ctx_t *api_ctx_new(const pal_papp_t *app, int argc, char **argv);
void api_ctx_free(api_ctx_t *c);

/* Little-endian fields in the app's memory (records and results): the layout the API documents, whatever the host. */
static inline void api_put16(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}
static inline uint32_t api_get16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8;
}
static inline void api_put32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}
static inline uint32_t api_get32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* State the core modules keep per app. Each module owns its own part; the runtime only initialises it (api_ctx_new)
 * and calls the release hooks (api_ctx_free) when the run ends. */
#define API_KIDS_MAX 8
typedef struct {
    int32_t ev_fd;              /* events.c: the process event queue's handle, -1 until pal_events() */
    int32_t kids[API_KIDS_MAX]; /* proc.c: children this app started and has not waited for; 0 when the slot is free */
    bool tty_saved;             /* tty.c: the app changed the terminal mode; tty_fd and tty_flags hold the mode before */
    int32_t tty_fd;
    uint32_t tty_flags;
    int32_t legend_fd; /* legend.c: the terminal the app published legends to, -1 when none */
    void *crypto; /* crypto.c: the process's digest, cipher and key tables (crypto_tab.c), NULL until first use */
} api_ext_t;
api_ext_t *api_ext(api_ctx_t *c);
void api_tty_size(int *cols, int *rows); /* tty.c: the controlling terminal's size, 80x24 when there is none */
void api_tty_restore(api_ctx_t *c);      /* tty.c: puts the terminal back as the app found it (called when the app ends) */
int api_ctty_fd(void);                   /* tty.c: the first of fd 0, 1, 2 that is a terminal, or -1 */
void api_legend_release(api_ctx_t *c);   /* legend.c: removes the app's legends from its terminal (called when the app ends) */
void api_kids_release(api_ctx_t *c);     /* proc.c: children left running are reaped by the system (called when the app ends) */

#endif
