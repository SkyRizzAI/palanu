/* Nema objects and the one waiting primitive.
 *
 * Everything a process can hold a handle to (pipe end, tty end, null device, file, socket) is a nema_obj_t:
 * a reference-counted object with a small operation table. Operations never block; blocking is built once,
 * on top of nema_wait_on(), so every waiting call in the system shares the same wake-up and signal rules.
 */
#ifndef NEMA_OBJ_H
#define NEMA_OBJ_H

#include "nema/mem.h"

#define NEMA_POLL_IN 0x1u  /* read would not block (data, or end of stream) */
#define NEMA_POLL_OUT 0x2u /* write would not block */
#define NEMA_POLL_HUP 0x4u /* the other side is gone */
#define NEMA_POLL_ERR 0x8u

typedef struct nema_obj nema_obj_t;
typedef struct nema_waiter_node nema_waiter_node_t;

typedef struct {
    const char *kind;
    /* Non-blocking. >0 bytes moved, 0 end of stream, NEMA_ERR_AGAIN if it would block, other <0 errors. */
    int (*read)(nema_obj_t *o, void *buf, size_t n);
    int (*write)(nema_obj_t *o, const void *buf, size_t n);
    uint32_t (*poll)(nema_obj_t *o, uint32_t events); /* returns the ready subset (plus HUP/ERR) */
    nema_err_t (*ioctl)(nema_obj_t *o, uint32_t req, void *arg);
    void (*destroy)(nema_obj_t *o); /* called when the last reference is dropped */
} nema_obj_ops_t;

struct nema_obj {
    const nema_obj_ops_t *ops;
    int32_t refs;                  /* atomic: ref/unref never take a lock, so they are safe under any lock */
    nema_waiter_node_t *waiters;   /* guarded by the wait lock */
    uint32_t flags;                /* NEMA_OBJ_* */
};

/* A write to this object is never cut short by a signal: nema_obj_write runs it inside nema_cancel_defer(). Set by file
 * objects, so a kill arriving mid-write is applied after the write (docs/adr/0003). */
#define NEMA_OBJ_ATOMIC_WRITE 0x1u
/* Reads and writes never wait: with no data (or no room) they return NEMA_ERR_AGAIN at once. Set by pal_set_nonblock. */
#define NEMA_OBJ_NONBLOCK 0x2u

void nema_obj_init(nema_obj_t *o, const nema_obj_ops_t *ops); /* refs = 1 */
nema_obj_t *nema_obj_ref(nema_obj_t *o);
/* Take a reference only if the object is still alive (refs > 0). For lists that outlive the last reference: an
 * object whose count reached zero stays listed until its destroy hook takes the list lock. */
bool nema_obj_try_ref(nema_obj_t *o);
void nema_obj_unref(nema_obj_t *o); /* NULL is fine */
/* Readiness of o for the events in the mask, without blocking: the ready subset (plus HUP/ERR), 0 for NULL. */
uint32_t nema_obj_poll(nema_obj_t *o, uint32_t events);

/* A device request on an object (a file's seek and size, NEMA_FIOC_*); NEMA_ERR_INVAL when the object has none. NULL is fine. */
nema_err_t nema_obj_ioctl(nema_obj_t *o, uint32_t req, void *arg);

void nema_obj_set_nonblock(nema_obj_t *o, bool on);
bool nema_obj_is_nonblock(const nema_obj_t *o);

/* Wake everyone waiting on o. Call with no object lock held. */
void nema_notify(nema_obj_t *o);

/* Wait until pred(ctx) is true. pred runs with the wait lock held: keep it short and take only object
 * locks inside it. Wake-ups come from nema_notify() on any of objs[] or from a fatal signal to the caller.
 * Returns NEMA_OK, NEMA_ERR_TIMEOUT, or NEMA_ERR_INTR. timeout_ms < 0 waits forever; 0 checks once. */
typedef bool (*nema_wait_pred_t)(void *ctx);
#define NEMA_WAIT_OBJS_MAX 16
int nema_wait_on(nema_obj_t *const *objs, size_t nobjs, nema_wait_pred_t pred, void *ctx, int32_t timeout_ms);

/* Blocking wrappers. timeout_ms < 0 waits forever, 0 never blocks. nema_obj_write blocks until all bytes
 * are written unless an error or timeout intervenes; it then returns the bytes written so far if any. */
int nema_obj_read(nema_obj_t *o, void *buf, size_t n, int32_t timeout_ms);
int nema_obj_write(nema_obj_t *o, const void *buf, size_t n, int32_t timeout_ms);

typedef struct {
    nema_obj_t *obj;
    uint32_t events;
    uint32_t revents;
} nema_poll_obj_t;
int nema_poll_objs(nema_poll_obj_t *fds, size_t n, int32_t timeout_ms); /* count ready, 0 timeout, <0 error */

/* Sleep that a fatal signal can cut short. Returns NEMA_OK after the time, or NEMA_ERR_INTR. */
int nema_sleep_ms(int32_t ms);

/* Per-task context: the wait semaphore and the owning process. */
struct nema_proc;
typedef struct {
    nema_port_sem_t *sem;
    struct nema_proc *proc; /* NULL for system tasks */
} nema_tctx_t;
nema_tctx_t *nema_tctx(void);
void nema_tctx_release(void); /* called by the task trampoline on exit */

/* A device that discards writes and reads as end of stream. */
nema_obj_t *nema_null_obj(void);

#endif
