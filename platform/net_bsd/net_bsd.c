/* Sockets for Nema on the BSD socket API (lwIP on the board, the OS on the host).
 *
 * Operations never block. Waiting is done by nema_wait_on(): a pump task select()s over the sockets that someone is
 * waiting on, and notifies the ones that became ready. Each object's poll() answers with a zero-timeout select().
 *
 * The pump sleeps in select() with no timeout. It is woken by a wake fd that is always in its select set:
 *   - host: a pipe(), both ends non-blocking;
 *   - board: an eventfd from the ESP-IDF VFS (select works on it next to lwIP sockets).
 * Anything that changes the set (a socket added or closed, a waiter arming a socket) writes one byte to the wake
 * fd, at most one per pump cycle. With no sockets in use the pump is blocked and wakes 0 times per second.
 *
 * Interest is one-shot: poll() arms a socket for IN or OUT, the pump selects it only while armed, and clears the
 * arm when it notifies. A waiter that is still not ready re-polls after waking and re-arms. So a socket nobody waits
 * on (unread data, a listener nobody accepts from) is never selected and cannot spin the pump. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ESP_PLATFORM)
#include "esp_vfs_eventfd.h"
#endif

#include "nema/nema.h"
#include "nema/net.h"

/* Only used if no wake fd can be made (eventfd registration failed on the board). Then a socket added while the
 * pump sleeps is seen after at most this long. Data on sockets already in the set still wakes the pump at once. */
#define PUMP_FALLBACK_MS 250
#define PUMP_SNAP_MAX 16

typedef struct sock {
    nema_obj_t obj;
    int fd;
    bool want_in;  /* a waiter is blocked on read readiness (one-shot, see the header) */
    bool want_out; /* a waiter is blocked on write readiness */
    bool listener;
    struct sock *next;
} sock_t;

static struct {
    nema_port_mutex_t *lock;
    sock_t *head;
    nema_owner_t owner;
    bool started;
    int wake_rd; /* -1 if no wake fd */
    int wake_wr;
    bool wake_pending; /* a wake byte is in the pipe, or about to be: skip further writes */
    uint32_t wakeups;  /* pump returns from select, for tests and diagnostics */
} N = {NULL, NULL, 0, false, -1, -1, false, 0};

/* ---- wake fd ---- */
static void wake_open(void) {
#if defined(ESP_PLATFORM)
    esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    if (esp_vfs_eventfd_register(&cfg) == ESP_OK) N.wake_rd = eventfd(0, 0);
    N.wake_wr = N.wake_rd;
#else
    int fds[2];
    if (pipe(fds) == 0) {
        N.wake_rd = fds[0];
        N.wake_wr = fds[1];
        fcntl(N.wake_rd, F_SETFL, fcntl(N.wake_rd, F_GETFL, 0) | O_NONBLOCK);
        fcntl(N.wake_wr, F_SETFL, fcntl(N.wake_wr, F_GETFL, 0) | O_NONBLOCK);
    }
#endif
}

/* Callable from any task, with any lock held except N.lock (it never blocks: the fd is non-blocking). */
static void pump_wake(void) {
    if (N.wake_wr < 0) return;
    if (__atomic_exchange_n(&N.wake_pending, true, __ATOMIC_ACQ_REL)) return;
    uint64_t one = 1;
    ssize_t w = write(N.wake_wr, &one, sizeof(one));
    (void)w;
}

/* Arm a socket for the events a waiter is blocked on. Only an idle-to-armed flip needs a wake. */
static void arm(sock_t *s, uint32_t events) {
    bool fresh = false;
    if (events & NEMA_POLL_IN) fresh |= !__atomic_exchange_n(&s->want_in, true, __ATOMIC_ACQ_REL);
    if (events & NEMA_POLL_OUT) fresh |= !__atomic_exchange_n(&s->want_out, true, __ATOMIC_ACQ_REL);
    if (fresh) pump_wake();
}

/* ---- the pump ---- */
static void pump_task(void *arg) {
    (void)arg;
    for (;;) {
        /* Clear the pending flag before reading the state: a wake that comes after this writes a byte again. */
        __atomic_store_n(&N.wake_pending, false, __ATOMIC_RELEASE);
        if (N.wake_rd >= 0) {
            uint64_t junk;
            ssize_t dr = read(N.wake_rd, &junk, sizeof(junk));
            (void)dr;
        }
        fd_set rs, ws;
        FD_ZERO(&rs);
        FD_ZERO(&ws);
        int maxfd = N.wake_rd;
        if (N.wake_rd >= 0) FD_SET(N.wake_rd, &rs);
        sock_t *snap[PUMP_SNAP_MAX];
        bool snap_in[PUMP_SNAP_MAX], snap_out[PUMP_SNAP_MAX];
        int ns = 0;
        nema_port_mutex_lock(N.lock);
        for (sock_t *s = N.head; s && ns < PUMP_SNAP_MAX; s = s->next) {
            if (!nema_obj_try_ref(&s->obj)) continue; /* being destroyed */
            bool in = __atomic_exchange_n(&s->want_in, false, __ATOMIC_ACQ_REL);
            bool out = __atomic_exchange_n(&s->want_out, false, __ATOMIC_ACQ_REL);
            if (in) FD_SET(s->fd, &rs);
            if (out) FD_SET(s->fd, &ws);
            if ((in || out) && s->fd > maxfd) maxfd = s->fd;
            snap[ns] = s;
            snap_in[ns] = in;
            snap_out[ns] = out;
            ns++;
        }
        nema_port_mutex_unlock(N.lock);

        struct timeval tv = {PUMP_FALLBACK_MS / 1000, (PUMP_FALLBACK_MS % 1000) * 1000};
        int r = select(maxfd + 1, &rs, &ws, NULL, N.wake_rd >= 0 ? NULL : &tv);
        __atomic_add_fetch(&N.wakeups, 1, __ATOMIC_RELAXED);

        for (int i = 0; i < ns; i++) {
            sock_t *s = snap[i];
            bool ready = r > 0 && ((snap_in[i] && FD_ISSET(s->fd, &rs)) || (snap_out[i] && FD_ISSET(s->fd, &ws)));
            if (ready) {
                nema_notify(&s->obj); /* the waiter re-polls and re-arms if it is still not ready */
            } else {
                /* Not ready, or woken for another reason: keep the arm for the next cycle. */
                if (snap_in[i]) __atomic_store_n(&s->want_in, true, __ATOMIC_RELEASE);
                if (snap_out[i]) __atomic_store_n(&s->want_out, true, __ATOMIC_RELEASE);
            }
            nema_obj_unref(&s->obj);
        }
    }
}

void nema_net_init(void) {
    if (N.started) return;
    N.started = true;
    N.lock = nema_port_mutex_create();
    N.owner = nema_owner_register("net");
    wake_open();
    nema_task_desc_t td = {.name = "net", .fn = pump_task, .stack = 3072, .prio = 5, .core = -1, .role = NEMA_TASK_SYSTEM, .owner = N.owner};
    nema_task_create(&td);
}

uint32_t nema_net_pump_wakeups(void) { return __atomic_load_n(&N.wakeups, __ATOMIC_RELAXED); }

/* ---- socket object ---- */
static int map_errno(int e) {
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS) return NEMA_ERR_AGAIN;
    if (e == EPIPE || e == ENOTCONN || e == ECONNABORTED) return NEMA_ERR_PIPE;
    if (e == ECONNRESET) return NEMA_ERR_RESET;
    if (e == ECONNREFUSED) return NEMA_ERR_REFUSED;
    if (e == ENETUNREACH || e == EHOSTUNREACH || e == EHOSTDOWN) return NEMA_ERR_UNREACH;
    if (e == ETIMEDOUT) return NEMA_ERR_TIMEOUT;
    if (e == EINTR) return NEMA_ERR_INTR;
    return NEMA_ERR_IO;
}

static int s_read(nema_obj_t *o, void *buf, size_t n) {
    sock_t *s = (sock_t *)o;
    ssize_t r = recv(s->fd, buf, n, MSG_DONTWAIT);
    if (r > 0) return (int)r;
    if (r == 0) return 0; /* orderly shutdown by the peer */
    return map_errno(errno);
}

static int s_write(nema_obj_t *o, const void *buf, size_t n) {
    sock_t *s = (sock_t *)o;
#ifdef MSG_NOSIGNAL
    ssize_t r = send(s->fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL);
#else
    ssize_t r = send(s->fd, buf, n, MSG_DONTWAIT);
#endif
    if (r >= 0) return r ? (int)r : NEMA_ERR_AGAIN;
    return map_errno(errno);
}

static uint32_t s_poll(nema_obj_t *o, uint32_t events) {
    sock_t *s = (sock_t *)o;
    fd_set rs, ws, es;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    FD_ZERO(&es);
    FD_SET(s->fd, &rs);
    FD_SET(s->fd, &ws);
    FD_SET(s->fd, &es);
    struct timeval tv = {0, 0};
    uint32_t r = 0;
    arm(s, events);
    if (select(s->fd + 1, &rs, &ws, &es, &tv) > 0) {
        if (FD_ISSET(s->fd, &rs)) r |= NEMA_POLL_IN; /* data, a new connection, or end of stream */
        if (FD_ISSET(s->fd, &ws)) r |= NEMA_POLL_OUT;
        if (FD_ISSET(s->fd, &es)) r |= NEMA_POLL_ERR;
    }
    return r;
}

static nema_err_t s_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    if (req == NEMA_NETIOC_GETFD) {
        *(int *)arg = ((sock_t *)o)->fd;
        return NEMA_OK;
    }
    if (req == NEMA_NETIOC_SHUTDOWN) {
        int how = *(int *)arg;
        int h = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : how == 2 ? SHUT_RDWR : -1;
        if (h < 0) return NEMA_ERR_INVAL;
        return shutdown(((sock_t *)o)->fd, h) == 0 ? NEMA_OK : map_errno(errno);
    }
    return NEMA_ERR_NOSYS;
}

static void s_destroy(nema_obj_t *o) {
    sock_t *s = (sock_t *)o;
    nema_port_mutex_lock(N.lock);
    for (sock_t **pp = &N.head; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    nema_port_mutex_unlock(N.lock);
    pump_wake(); /* the pump rebuilds its set without this socket */
    close(s->fd);
    nema_free(s);
}

static const nema_obj_ops_t SOCK_OPS = {"socket", s_read, s_write, s_poll, s_ioctl, s_destroy};

static nema_err_t wrap(int fd, bool listener, nema_obj_t **out) {
    sock_t *s = (sock_t *)nema_calloc(NEMA_MEM_ANY, N.owner, sizeof(*s));
    if (!s) {
        close(fd);
        return NEMA_ERR_NOMEM;
    }
    nema_obj_init(&s->obj, &SOCK_OPS);
    s->fd = fd;
    s->listener = listener;
    nema_port_mutex_lock(N.lock);
    s->next = N.head;
    N.head = s;
    nema_port_mutex_unlock(N.lock);
    pump_wake();
    *out = &s->obj;
    return NEMA_OK;
}

static int set_nonblock(int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

/* ---- name lookup ----
 * getaddrinfo() cannot be interrupted or given a timeout, so it runs in a helper task. The caller waits in
 * nema_wait_on() on the job's object (a fatal signal or the timeout ends the wait); the helper notifies that object
 * when it has an answer. The job is freed by whichever side lets go last, so a caller that gave up does not leave the
 * helper with freed memory. A helper stuck in the resolver stays until the resolver returns (bounded by its own
 * timeouts). */
#define RESOLVE_HOST_MAX 128
#define RESOLVE_STACK 6144u

typedef struct {
    nema_obj_t obj; /* the caller waits on this; the helper notifies it */
    char host[RESOLVE_HOST_MAX];
    uint8_t ip[4];
    nema_err_t err;
    bool done;
    int32_t refs; /* the caller and the helper */
} resolve_job_t;

static int job_nosys(nema_obj_t *o, void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_NOSYS;
}
static int job_write(nema_obj_t *o, const void *buf, size_t n) {
    (void)o, (void)buf, (void)n;
    return NEMA_ERR_NOSYS;
}
static uint32_t job_poll(nema_obj_t *o, uint32_t events) {
    (void)o, (void)events;
    return 0;
}
static nema_err_t job_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    (void)o, (void)req, (void)arg;
    return NEMA_ERR_NOSYS;
}
static const nema_obj_ops_t JOB_OPS = {"lookup", job_nosys, job_write, job_poll, job_ioctl, NULL};

static void job_release(resolve_job_t *j) {
    if (__atomic_sub_fetch(&j->refs, 1, __ATOMIC_ACQ_REL) == 0) nema_free(j);
}

static void resolve_task(void *arg) {
    resolve_job_t *j = (resolve_job_t *)arg;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    j->err = NEMA_ERR_NOENT;
    if (getaddrinfo(j->host, NULL, &hints, &res) == 0 && res) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)res->ai_addr;
        memcpy(j->ip, &sin->sin_addr, 4);
        j->err = NEMA_OK;
    }
    if (res) freeaddrinfo(res);
    __atomic_store_n(&j->done, true, __ATOMIC_RELEASE);
    nema_notify(&j->obj);
    job_release(j);
}

static bool job_done(void *ctx) { return __atomic_load_n(&((resolve_job_t *)ctx)->done, __ATOMIC_ACQUIRE); }

nema_err_t nema_net_resolve_addr(const char *host, int32_t timeout_ms, uint8_t ip[4]) {
    if (!host || !*host) return NEMA_ERR_INVAL;
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) { /* a dotted address needs no lookup */
        memcpy(ip, &a, 4);
        return NEMA_OK;
    }
    if (strlen(host) >= RESOLVE_HOST_MAX) return NEMA_ERR_RANGE;
    resolve_job_t *j = (resolve_job_t *)nema_calloc(NEMA_MEM_ANY, N.owner, sizeof(*j));
    if (!j) return NEMA_ERR_NOMEM;
    nema_obj_init(&j->obj, &JOB_OPS);
    nema_strlcpy(j->host, host, sizeof(j->host));
    j->err = NEMA_ERR_NOENT;
    j->refs = 2;
    nema_task_desc_t td = {.name = "resolve", .fn = resolve_task, .arg = j, .stack = RESOLVE_STACK, .prio = 5,
                           .core = -1, .role = NEMA_TASK_SYSTEM, .owner = N.owner};
    if (nema_task_create(&td) != NEMA_OK) {
        nema_free(j);
        return NEMA_ERR_NOMEM;
    }
    nema_obj_t *objs[1] = {&j->obj};
    int w = nema_wait_on(objs, 1, job_done, j, timeout_ms);
    nema_err_t r = w;
    if (w == NEMA_OK) {
        r = j->err;
        if (r == NEMA_OK) memcpy(ip, j->ip, 4);
    }
    job_release(j);
    return r;
}

nema_err_t nema_net_resolve(const char *host, char *ip, size_t cap) {
    uint8_t b[4];
    nema_err_t r = nema_net_resolve_addr(host, -1, b);
    if (r != NEMA_OK) return r;
    snprintf(ip, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return NEMA_OK;
}

nema_err_t nema_net_connect(const char *host, uint16_t port, int32_t timeout_ms, nema_obj_t **out) {
    uint64_t deadline = timeout_ms >= 0 ? nema_port_time_us() + (uint64_t)timeout_ms * 1000u : 0;
    uint8_t ip[4];
    nema_err_t err = nema_net_resolve_addr(host, timeout_ms, ip);
    if (err != NEMA_OK) return err;
    if (timeout_ms >= 0 && nema_port_time_us() >= deadline) return NEMA_ERR_TIMEOUT;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NEMA_ERR_NOMEM;
    set_nonblock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, ip, 4);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    err = NEMA_OK;
    if (r < 0 && errno != EINPROGRESS) {
        err = (nema_err_t)map_errno(errno);
    } else if (r < 0) {
        for (;;) {
            fd_set ws;
            FD_ZERO(&ws);
            FD_SET(fd, &ws);
            struct timeval tv = {0, 100000};
            int sr = select(fd + 1, NULL, &ws, NULL, &tv);
            if (sr > 0) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                err = soerr ? (nema_err_t)map_errno(soerr) : NEMA_OK;
                break;
            }
            if (nema_interrupted()) {
                err = NEMA_ERR_INTR;
                break;
            }
            if (timeout_ms >= 0 && nema_port_time_us() >= deadline) {
                err = NEMA_ERR_TIMEOUT;
                break;
            }
        }
    }
    if (err != NEMA_OK) {
        close(fd);
        return err;
    }
    return wrap(fd, false, out);
}

nema_err_t nema_net_listen_bl(uint16_t port, int backlog, nema_obj_t **out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NEMA_ERR_NOMEM;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, backlog) < 0) {
        close(fd);
        return NEMA_ERR_BUSY;
    }
    set_nonblock(fd);
    return wrap(fd, true, out);
}

nema_err_t nema_net_listen(uint16_t port, nema_obj_t **out) { return nema_net_listen_bl(port, 2, out); }

nema_err_t nema_net_accept(nema_obj_t *listener, int32_t timeout_ms, nema_obj_t **client) {
    if (!listener || listener->ops != &SOCK_OPS || !((sock_t *)listener)->listener) return NEMA_ERR_INVAL;
    sock_t *l = (sock_t *)listener;
    int64_t deadline = (int64_t)nema_port_time_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int fd = accept(l->fd, NULL, NULL);
        if (fd >= 0) {
            set_nonblock(fd);
            return wrap(fd, false, client);
        }
        int32_t left = -1;
        if (timeout_ms >= 0) {
            int64_t now = (int64_t)nema_port_time_us();
            if (now >= deadline) return NEMA_ERR_TIMEOUT;
            left = (int32_t)((deadline - now + 999) / 1000);
        }
        /* Sleep until the listener has a connection (the pump selects it while we wait). */
        nema_poll_obj_t p = {listener, NEMA_POLL_IN, 0};
        int r = nema_poll_objs(&p, 1, left);
        if (r < 0) return NEMA_ERR_INTR;
    }
}
