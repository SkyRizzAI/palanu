#include <string.h>

#include "internal.h"
#include "nema/ring.h"

#define LINE_MAX_LEN 120
#define IN_CAP 256
#define OUT_CAP 2048

typedef struct {
    nema_obj_t obj;
    nema_tty_t *t;
} end_t;

struct nema_tty {
    nema_port_mutex_t *lock;
    nema_ring_t in, out;
    end_t proc_end, term_end;
    uint32_t mode;
    nema_winsize_t ws;
    nema_pid_t fg;
    uint8_t line[LINE_MAX_LEN];
    size_t linelen;
    bool eof;    /* Ctrl-D on an empty line: the next read returns 0 once */
    bool closed; /* the terminal side is gone */
    int live_ends;
    bool edit_on; /* the shell is editing a line (see nema_tty_set_edit) */
    size_t edit_len, edit_cursor;
    char edit[NEMA_TTY_EDIT_MAX];
    uint32_t legend_gen; /* bumped by every SETLEGEND */
    char legend[NEMA_TTY_LEGEND_MAX];
};

/* Lock held. Cuts at a UTF-8 boundary so the terminal never sees half a character. */
static void edit_set_locked(nema_tty_t *t, const char *s, size_t len, size_t cursor) {
    size_t n = len < NEMA_TTY_EDIT_MAX ? len : NEMA_TTY_EDIT_MAX;
    if (n < len)
        while (n > 0 && ((uint8_t)s[n] & 0xC0u) == 0x80u) n--;
    memcpy(t->edit, s, n);
    t->edit_len = n;
    t->edit_cursor = cursor < n ? cursor : n;
    t->edit_on = true;
}

/* ---- output path: process -> terminal ---- */
static void out_put(nema_tty_t *t, const char *s, size_t n) { /* lock held; drops on overflow (echo only) */
    nema_ring_write(&t->out, s, n);
}

static int proc_write(nema_obj_t *o, const void *buf, size_t n) {
    nema_tty_t *t = ((end_t *)o)->t;
    const uint8_t *b = (const uint8_t *)buf;
    size_t used = 0;
    nema_port_mutex_lock(t->lock);
    if (t->closed) {
        nema_port_mutex_unlock(t->lock);
        return (int)n; /* nobody is watching: discard */
    }
    while (used < n) {
        bool crlf = (t->mode & NEMA_TTY_ONLCR) && b[used] == '\n';
        if (nema_ring_space(&t->out) < (crlf ? 2u : 1u)) break;
        if (crlf) nema_ring_write(&t->out, "\r", 1);
        nema_ring_write(&t->out, &b[used], 1);
        used++;
    }
    nema_port_mutex_unlock(t->lock);
    if (used) nema_notify(&t->term_end.obj);
    return used ? (int)used : NEMA_ERR_AGAIN;
}

/* ---- input path: terminal -> process ---- */
static int proc_read(nema_obj_t *o, void *buf, size_t n) {
    nema_tty_t *t = ((end_t *)o)->t;
    nema_port_mutex_lock(t->lock);
    int r;
    if (nema_ring_count(&t->in) > 0) {
        r = (int)nema_ring_read(&t->in, buf, n);
    } else if (t->eof) {
        t->eof = false;
        r = 0;
    } else if (t->closed) {
        r = 0;
    } else {
        r = NEMA_ERR_AGAIN;
    }
    nema_port_mutex_unlock(t->lock);
    return r;
}

static uint32_t proc_poll(nema_obj_t *o, uint32_t ev) {
    nema_tty_t *t = ((end_t *)o)->t;
    uint32_t r = 0;
    nema_port_mutex_lock(t->lock);
    if (nema_ring_count(&t->in) > 0 || t->eof || t->closed) r |= NEMA_POLL_IN;
    if (nema_ring_space(&t->out) >= 2 || t->closed) r |= NEMA_POLL_OUT;
    if (t->closed) r |= NEMA_POLL_HUP;
    nema_port_mutex_unlock(t->lock);
    (void)ev;
    return r;
}

static nema_err_t proc_ioctl(nema_obj_t *o, uint32_t req, void *arg) {
    nema_tty_t *t = ((end_t *)o)->t;
    nema_err_t r = NEMA_OK;
    nema_port_mutex_lock(t->lock);
    switch (req) {
    case NEMA_TIOC_GETSIZE: *(nema_winsize_t *)arg = t->ws; break;
    case NEMA_TIOC_GETMODE: *(uint32_t *)arg = t->mode; break;
    case NEMA_TIOC_SETMODE:
        t->mode = *(uint32_t *)arg;
        if (!(t->mode & NEMA_TTY_ICANON)) {
            t->linelen = 0;
            t->edit_on = false; /* a program took the terminal in raw mode: no line is being edited any more */
        }
        break;
    case NEMA_TIOC_SETFG: t->fg = *(nema_pid_t *)arg; break;
    case NEMA_TIOC_SETEDIT: {
        const nema_edit_t *e = (const nema_edit_t *)arg;
        if (!e || (!e->utf8 && e->len)) {
            r = NEMA_ERR_INVAL;
            break;
        }
        edit_set_locked(t, e->utf8, e->len, e->cursor);
        break;
    }
    case NEMA_TIOC_CLREDIT: t->edit_on = false; break;
    case NEMA_TIOC_SETLEGEND: {
        const char *text = (const char *)arg;
        size_t n = text ? strlen(text) : 0;
        if (n >= NEMA_TTY_LEGEND_MAX) {
            r = NEMA_ERR_INVAL;
            break;
        }
        if (n) memcpy(t->legend, text, n);
        t->legend[n] = 0;
        t->legend_gen++;
        break;
    }
    default: r = NEMA_ERR_NOSYS;
    }
    nema_port_mutex_unlock(t->lock);
    return r;
}

static void end_destroy(nema_obj_t *o) {
    nema_tty_t *t = ((end_t *)o)->t;
    nema_port_mutex_lock(t->lock);
    bool last = --t->live_ends == 0;
    nema_port_mutex_unlock(t->lock);
    if (last) {
        nema_port_mutex_destroy(t->lock);
        nema_free(t);
    }
}

/* Terminal side: reads what the process wrote. */
static int term_read(nema_obj_t *o, void *buf, size_t n) {
    nema_tty_t *t = ((end_t *)o)->t;
    nema_port_mutex_lock(t->lock);
    int r;
    if (nema_ring_count(&t->out) > 0) r = (int)nema_ring_read(&t->out, buf, n);
    else if (t->closed) r = 0;
    else r = NEMA_ERR_AGAIN;
    nema_port_mutex_unlock(t->lock);
    if (r > 0) nema_notify(&t->proc_end.obj); /* space freed for a blocked writer */
    return r;
}

static int term_write(nema_obj_t *o, const void *b, size_t n) {
    (void)o, (void)b, (void)n;
    return NEMA_ERR_PERM; /* keys go in through nema_tty_input() so the line discipline sees them */
}

static uint32_t term_poll(nema_obj_t *o, uint32_t ev) {
    nema_tty_t *t = ((end_t *)o)->t;
    uint32_t r = 0;
    nema_port_mutex_lock(t->lock);
    if (nema_ring_count(&t->out) > 0 || t->closed) r |= NEMA_POLL_IN;
    nema_port_mutex_unlock(t->lock);
    (void)ev;
    return r;
}

static nema_err_t term_ioctl(nema_obj_t *o, uint32_t r, void *a) {
    (void)o, (void)r, (void)a;
    return NEMA_ERR_NOSYS;
}

static const nema_obj_ops_t PROC_OPS = {"tty", proc_read, proc_write, proc_poll, proc_ioctl, end_destroy};
static const nema_obj_ops_t TERM_OPS = {"tty-term", term_read, term_write, term_poll, term_ioctl, end_destroy};

nema_err_t nema_tty_create(nema_tty_t **out, uint16_t cols, uint16_t rows, nema_owner_t owner) {
    /* Shared by reference count between the process end and the terminal end: never process-owned. */
    nema_tty_t *t = (nema_tty_t *)nema_alloc_ex(NEMA_MEM_ANY, nema_mem_shared_owner(owner), sizeof(*t) + IN_CAP + OUT_CAP, 0,
                                                NEMA_ALLOC_ZERO);
    if (!t) return NEMA_ERR_NOMEM;
    t->lock = nema_port_mutex_create();
    if (!t->lock) {
        nema_free(t);
        return NEMA_ERR_NOMEM;
    }
    nema_ring_init(&t->in, (uint8_t *)(t + 1), IN_CAP);
    nema_ring_init(&t->out, (uint8_t *)(t + 1) + IN_CAP, OUT_CAP);
    t->proc_end.t = t->term_end.t = t;
    nema_obj_init(&t->proc_end.obj, &PROC_OPS);
    nema_obj_init(&t->term_end.obj, &TERM_OPS);
    t->live_ends = 2;
    t->mode = NEMA_TTY_COOKED;
    t->ws.cols = cols;
    t->ws.rows = rows;
    *out = t;
    return NEMA_OK;
}

void nema_tty_destroy(nema_tty_t *t) {
    nema_port_mutex_lock(t->lock);
    t->closed = true;
    nema_port_mutex_unlock(t->lock);
    nema_notify(&t->proc_end.obj);
    nema_notify(&t->term_end.obj);
    /* Drop the owner's references. Processes still holding handles keep the proc end alive. */
    nema_obj_unref(&t->term_end.obj);
    nema_obj_unref(&t->proc_end.obj);
}

nema_obj_t *nema_tty_proc_obj(nema_tty_t *t) { return &t->proc_end.obj; }
nema_obj_t *nema_tty_term_obj(nema_tty_t *t) { return &t->term_end.obj; }

/* Echo helpers: lock held. */
static void echo(nema_tty_t *t, const char *s, size_t n) {
    if (t->mode & NEMA_TTY_ECHO) out_put(t, s, n);
}

static void commit_line(nema_tty_t *t, bool newline) {
    if (newline && t->linelen < sizeof(t->line)) t->line[t->linelen++] = '\n';
    nema_ring_write(&t->in, t->line, t->linelen);
    t->linelen = 0;
}

int nema_tty_input(nema_tty_t *t, const void *bytes, size_t n) {
    const uint8_t *b = (const uint8_t *)bytes;
    nema_pid_t int_target = 0;
    bool wake_in = false, wake_out = false;

    nema_port_mutex_lock(t->lock);
    if (t->closed) {
        nema_port_mutex_unlock(t->lock);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if ((t->mode & NEMA_TTY_ISIG) && c == 0x03) {
            int_target = t->fg;
            if (t->mode & NEMA_TTY_ICANON) {
                t->linelen = 0;
                echo(t, "^C\r\n", 4);
                wake_out = true;
            }
            continue;
        }
        if (!(t->mode & NEMA_TTY_ICANON)) {
            nema_ring_write(&t->in, &c, 1); /* raw: drop when full rather than block the keyboard */
            echo(t, (const char *)&c, 1);
            wake_in = true;
            continue;
        }
        switch (c) {
        case '\r':
        case '\n':
            echo(t, "\r\n", 2);
            commit_line(t, true);
            wake_in = wake_out = true;
            break;
        case 0x7f:
        case 0x08:
            if (t->linelen) {
                t->linelen--;
                echo(t, "\b \b", 3);
                wake_out = true;
            }
            break;
        case 0x15: /* Ctrl-U */
            while (t->linelen) {
                t->linelen--;
                echo(t, "\b \b", 3);
            }
            wake_out = true;
            break;
        case 0x04: /* Ctrl-D */
            if (t->linelen == 0) t->eof = true;
            else commit_line(t, false);
            wake_in = true;
            break;
        default:
            if (c >= 0x20 && t->linelen < sizeof(t->line) - 1) {
                t->line[t->linelen++] = c;
                echo(t, (const char *)&c, 1);
                wake_out = true;
            }
        }
    }
    nema_port_mutex_unlock(t->lock);
    if (int_target > 0) nema_proc_signal_group(int_target, NEMA_SIG_INT);
    if (wake_in) nema_notify(&t->proc_end.obj);
    if (wake_out) nema_notify(&t->term_end.obj);
    return (int)n;
}

int nema_tty_output_read(nema_tty_t *t, void *buf, size_t n, int32_t timeout_ms) {
    return nema_obj_read(&t->term_end.obj, buf, n, timeout_ms);
}

void nema_tty_set_size(nema_tty_t *t, uint16_t cols, uint16_t rows) {
    nema_pid_t fg;
    nema_port_mutex_lock(t->lock);
    t->ws.cols = cols;
    t->ws.rows = rows;
    fg = t->fg;
    nema_port_mutex_unlock(t->lock);
    if (fg > 0) nema_proc_signal_group(fg, NEMA_SIG_WINCH);
}

void nema_tty_set_foreground(nema_tty_t *t, nema_pid_t pid) {
    nema_port_mutex_lock(t->lock);
    t->fg = pid;
    nema_port_mutex_unlock(t->lock);
}

void nema_tty_set_edit(nema_tty_t *t, const char *utf8, size_t len, size_t cursor) {
    nema_port_mutex_lock(t->lock);
    edit_set_locked(t, utf8, len, cursor);
    nema_port_mutex_unlock(t->lock);
}

int nema_tty_get_edit(nema_tty_t *t, char *out, size_t cap, size_t *cursor) {
    int r = -1;
    nema_port_mutex_lock(t->lock);
    if (t->edit_on && cap > 0) {
        size_t n = t->edit_len < cap - 1 ? t->edit_len : cap - 1;
        while (n > 0 && n < t->edit_len && ((uint8_t)t->edit[n] & 0xC0u) == 0x80u) n--;
        memcpy(out, t->edit, n);
        out[n] = 0;
        if (cursor) *cursor = t->edit_cursor < n ? t->edit_cursor : n;
        r = (int)n;
    } else if (t->edit_on) {
        r = 0;
    }
    nema_port_mutex_unlock(t->lock);
    return r;
}

void nema_tty_clear_edit(nema_tty_t *t) {
    nema_port_mutex_lock(t->lock);
    t->edit_on = false;
    nema_port_mutex_unlock(t->lock);
}

uint32_t nema_tty_get_legend(nema_tty_t *t, char *out, size_t cap) {
    nema_port_mutex_lock(t->lock);
    if (cap > 0) nema_strlcpy(out, t->legend, cap);
    uint32_t gen = t->legend_gen;
    nema_port_mutex_unlock(t->lock);
    return gen;
}
