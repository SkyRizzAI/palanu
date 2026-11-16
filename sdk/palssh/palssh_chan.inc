/* palssh_chan.inc: the session channel (RFC 4254): open, pty-req, shell or exec, data both ways with window accounting,
 * window-change on resize, exit status, keepalive answers, and the `~.` escape at the start of a line. */

#define PSSH_STDIN_CHUNK 2048u
#define PSSH_WIN_ADJUST_AT 4096u

static int chan_send_data_room(const pssh_t *s) {
    return PSSH_BUF - s->out_len >= PSSH_STDIN_CHUNK + 64 && s->out_len < PSSH_BUF;
}

int pssh_session_open(pssh_t *s, const char *command, int cols, int rows) {
    uint8_t m[256];
    pssh_wb_t b;
    s->cols = cols;
    s->rows = rows;
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_CHANNEL_OPEN);
    wb_cstr(&b, "session");
    wb_u32(&b, 0); /* our channel number */
    wb_u32(&b, PSSH_WINDOW);
    wb_u32(&b, PSSH_CHAN_MAXPKT);
    if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    int64_t deadline = s->ops.now_ms(s->ops.ctx) + 15000;
    for (;;) {
        const uint8_t *p = 0;
        size_t n = 0;
        int rc = auth_recv(s, deadline, &p, &n);
        if (rc != PSSH_OK) return rc;
        if (p[0] == SSH_MSG_CHANNEL_OPEN_FAILURE) return PSSH_E_CHANNEL;
        if (p[0] == SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
            pssh_rb_t r;
            rb_init(&r, p + 1, n - 1);
            rb_u32(&r); /* our id */
            s->chan_remote = rb_u32(&r);
            s->win_remote = rb_u32(&r);
            s->maxpkt_remote = rb_u32(&r);
            if (r.err || s->maxpkt_remote == 0) return PSSH_E_PROTO;
            break;
        }
        return PSSH_E_PROTO;
    }
    s->chan_open = 1;
    if (s->want_pty) {
        wb_init(&b, m, sizeof m);
        wb_u8(&b, SSH_MSG_CHANNEL_REQUEST);
        wb_u32(&b, s->chan_remote);
        wb_cstr(&b, "pty-req");
        wb_u8(&b, 0);
        wb_cstr(&b, s->term[0] ? s->term : "xterm-256color");
        wb_u32(&b, (uint32_t)cols);
        wb_u32(&b, (uint32_t)rows);
        wb_u32(&b, 0);
        wb_u32(&b, 0);
        wb_str(&b, "", 1); /* encoded modes: a single TTY_OP_END byte */
        if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    }
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_CHANNEL_REQUEST);
    wb_u32(&b, s->chan_remote);
    if (command && command[0]) {
        wb_cstr(&b, "exec");
        wb_u8(&b, 1);
        wb_cstr(&b, command);
    } else {
        wb_cstr(&b, "shell");
        wb_u8(&b, 1);
    }
    if (b.err || pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
    return PSSH_OK;
}

static int chan_send_window_change(pssh_t *s) {
    uint8_t m[64];
    pssh_wb_t b;
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_CHANNEL_REQUEST);
    wb_u32(&b, s->chan_remote);
    wb_cstr(&b, "window-change");
    wb_u8(&b, 0);
    wb_u32(&b, (uint32_t)s->cols);
    wb_u32(&b, (uint32_t)s->rows);
    wb_u32(&b, 0);
    wb_u32(&b, 0);
    if (b.err) return PSSH_E_ARG;
    return pssh_send(s, m, b.n);
}

static int chan_send_adjust(pssh_t *s) {
    uint8_t m[16];
    pssh_wb_t b;
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    wb_u32(&b, s->chan_remote);
    wb_u32(&b, s->win_adjust_due);
    s->win_adjust_due = 0;
    return pssh_send(s, m, b.n);
}

static void stdout_all(pssh_t *s, int fd, const uint8_t *d, size_t n) {
    while (n) {
        int w = s->ops.stdout_write(s->ops.ctx, fd, d, n);
        if (w <= 0) return; /* the terminal went away: the data is dropped, the session goes on */
        d += w;
        n -= (size_t)w;
    }
}

/* Handles one message of the session. Sets chan_closed when the server closes the channel. */
static int chan_dispatch(pssh_t *s, const uint8_t *p, size_t n) {
    pssh_rb_t r;
    rb_init(&r, p + 1, n - 1);
    switch (p[0]) {
    case SSH_MSG_CHANNEL_DATA:
    case SSH_MSG_CHANNEL_EXTENDED_DATA: {
        int fd = 1;
        if (p[0] == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            rb_u32(&r);
            fd = 2;
        }
        rb_u32(&r);
        pssh_bytes_t d = rb_str(&r);
        if (r.err) return PSSH_E_PROTO;
        stdout_all(s, fd, d.p, d.n);
        s->win_adjust_due += (uint32_t)d.n;
        return PSSH_OK;
    }
    case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
        rb_u32(&r);
        uint32_t add = rb_u32(&r);
        if (r.err) return PSSH_E_PROTO;
        if (s->win_remote + add < s->win_remote) return PSSH_E_PROTO; /* overflow */
        s->win_remote += add;
        return PSSH_OK;
    }
    case SSH_MSG_CHANNEL_EOF:
        s->chan_eof_in = 1;
        return PSSH_OK;
    case SSH_MSG_CHANNEL_CLOSE: {
        if (!s->sent_close) {
            uint8_t c[8];
            pssh_wb_t b;
            wb_init(&b, c, sizeof c);
            wb_u8(&b, SSH_MSG_CHANNEL_CLOSE);
            wb_u32(&b, s->chan_remote);
            s->sent_close = 1;
            if (pssh_send(s, c, b.n) != PSSH_OK) return PSSH_E_IO;
        }
        s->chan_closed = 1;
        return PSSH_OK;
    }
    case SSH_MSG_CHANNEL_REQUEST: {
        rb_u32(&r);
        pssh_bytes_t type = rb_str(&r);
        int want = rb_bool(&r);
        if (r.err) return PSSH_E_PROTO;
        if (bytes_is(type, "exit-status")) {
            s->exit_status = (int)rb_u32(&r);
            s->have_exit = !r.err;
        } else if (bytes_is(type, "exit-signal")) {
            s->exit_status = 128;
            s->have_exit = 1;
        } else if (want) {
            uint8_t f = SSH_MSG_CHANNEL_FAILURE;
            uint8_t m[8];
            pssh_wb_t b;
            wb_init(&b, m, sizeof m);
            wb_u8(&b, f);
            wb_u32(&b, s->chan_remote);
            if (pssh_send(s, m, b.n) != PSSH_OK) return PSSH_E_IO;
        }
        return PSSH_OK;
    }
    case SSH_MSG_CHANNEL_SUCCESS:
        s->chan_req_ok = 1;
        return PSSH_OK;
    case SSH_MSG_CHANNEL_FAILURE:
        s->chan_req_fail = 1;
        return PSSH_E_CHANNEL;
    case SSH_MSG_GLOBAL_REQUEST: {
        rb_str(&r);
        int want = rb_bool(&r);
        if (want) {
            uint8_t f = SSH_MSG_REQUEST_FAILURE;
            if (pssh_send(s, &f, 1) != PSSH_OK) return PSSH_E_IO;
        }
        return PSSH_OK;
    }
    case SSH_MSG_IGNORE:
    case SSH_MSG_DEBUG:
    case SSH_MSG_UNIMPLEMENTED:
    case SSH_MSG_EXT_INFO:
        return PSSH_OK;
    case SSH_MSG_DISCONNECT: {
        rb_u32(&r);
        pssh_bytes_t text = rb_str(&r);
        size_t k = text.n < sizeof s->banner - 1 ? text.n : sizeof s->banner - 1;
        pssh_copy(s->banner, text.p, k);
        s->banner[k] = 0;
        return PSSH_E_EOF;
    }
    default:
        return PSSH_E_PROTO;
    }
}

/* Reads what the user typed and sends it. `~.` at the start of a line ends the session (when a terminal was asked
 * for). Returns PSSH_OK, or an error when the connection is gone. */
static int stdin_pump(pssh_t *s) {
    if (s->chan_eof_sent || s->win_remote == 0 || !chan_send_data_room(s)) return PSSH_OK;
    uint8_t in[PSSH_STDIN_CHUNK];
    size_t want = s->win_remote < PSSH_STDIN_CHUNK ? s->win_remote : PSSH_STDIN_CHUNK;
    if (want > s->maxpkt_remote) want = s->maxpkt_remote;
    int r = s->ops.stdin_read(s->ops.ctx, in, want);
    if (r == PSSH_E_AGAIN) return PSSH_OK;
    if (r < 0) return PSSH_E_IO;
    if (r == 0) {
        uint8_t e[8];
        pssh_wb_t b;
        wb_init(&b, e, sizeof e);
        wb_u8(&b, SSH_MSG_CHANNEL_EOF);
        wb_u32(&b, s->chan_remote);
        s->chan_eof_sent = 1;
        return pssh_send(s, e, b.n);
    }
    uint8_t out[PSSH_STDIN_CHUNK + 2];
    size_t o = 0;
    for (int i = 0; i < r; i++) {
        uint8_t c = in[i];
        if (s->want_pty && s->esc_pending) {
            s->esc_pending = 0;
            if (c == '.') {
                s->esc_quit = 1;
                break;
            }
            out[o++] = '~';
        } else if (s->want_pty && s->at_bol && c == '~') {
            s->esc_pending = 1;
            s->at_bol = 0;
            continue;
        }
        out[o++] = c;
        s->at_bol = (c == '\r' || c == '\n');
    }
    if (o == 0) return PSSH_OK;
    uint8_t m[PSSH_STDIN_CHUNK + 16];
    pssh_wb_t b;
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_CHANNEL_DATA);
    wb_u32(&b, s->chan_remote);
    wb_str(&b, out, o);
    if (b.err) return PSSH_E_ARG;
    int rc = pkt_encode(s, m, b.n);
    if (rc != PSSH_OK) return rc;
    s->win_remote -= (uint32_t)o;
    return PSSH_OK;
}

/* Sends CLOSE and DISCONNECT as far as the connection allows (best effort). */
static void chan_finish(pssh_t *s, uint32_t reason, const char *text) {
    if (s->chan_open && !s->sent_close) {
        uint8_t c[8];
        pssh_wb_t b;
        wb_init(&b, c, sizeof c);
        wb_u8(&b, SSH_MSG_CHANNEL_CLOSE);
        wb_u32(&b, s->chan_remote);
        s->sent_close = 1;
        if (pssh_send(s, c, b.n) == PSSH_OK) pssh_flush(s);
    }
    pssh_disconnect(s, reason, text);
}

void pssh_disconnect(pssh_t *s, uint32_t reason, const char *text) {
    if (s->disconnected) return;
    uint8_t m[256];
    pssh_wb_t b;
    wb_init(&b, m, sizeof m);
    wb_u8(&b, SSH_MSG_DISCONNECT);
    wb_u32(&b, reason);
    wb_cstr(&b, text);
    wb_cstr(&b, "");
    s->disconnected = 1;
    if (!b.err && pkt_encode(s, m, b.n) == PSSH_OK) {
        for (int i = 0; i < 8 && pssh_flush(s) == PSSH_E_AGAIN; i++) {
            unsigned got = 0;
            if (wait_io(s, PSSH_W_SOCK_OUT, 200, &got) == PSSH_E_IO) break;
        }
    }
}

int pssh_session_run(pssh_t *s) {
    if (!s->chan_open) return PSSH_E_CHANNEL;
    for (;;) {
        /* messages that arrived */
        for (;;) {
            const uint8_t *p = 0;
            size_t n = 0;
            int rc = pssh_recv_try(s, &p, &n);
            if (rc == PSSH_E_AGAIN) break;
            if (rc != PSSH_OK || n == 0) {
                pssh_disconnect(s, 2, "protocol error");
                return PSSH_E_PROTO;
            }
            rc = chan_dispatch(s, p, n);
            if (rc == PSSH_E_EOF) {
                return PSSH_E_EOF; /* the server disconnected */
            }
            if (rc != PSSH_OK) {
                pssh_disconnect(s, 2, "protocol error");
                return rc;
            }
            if (s->chan_closed) break;
        }
        if (s->chan_closed) break;
        if (s->win_adjust_due >= PSSH_WIN_ADJUST_AT && chan_send_adjust(s) != PSSH_OK) return PSSH_E_IO;
        if (s->resized) {
            s->resized = 0;
            if (chan_send_window_change(s) != PSSH_OK) return PSSH_E_IO;
        }
        if (s->quit) {
            chan_finish(s, 11, "stopped by the user");
            return PSSH_E_QUIT;
        }
        if (s->esc_quit) {
            chan_finish(s, 11, "disconnected by the user");
            return 255;
        }
        int rc = pssh_flush(s);
        if (rc == PSSH_E_IO) return PSSH_E_IO;
        unsigned want = PSSH_W_SOCK_IN;
        if (s->out_len > s->out_off) want |= PSSH_W_SOCK_OUT;
        if (!s->chan_eof_sent && s->win_remote > 0 && chan_send_data_room(s)) want |= PSSH_W_STDIN;
        unsigned got = 0;
        rc = wait_io(s, want, 500, &got);
        if (rc == PSSH_E_QUIT) {
            chan_finish(s, 11, "stopped by the user");
            return PSSH_E_QUIT;
        }
        if (rc != PSSH_OK) return rc;
        if (got & (PSSH_W_SOCK_IN | PSSH_W_SOCK_OUT)) {
            rc = io_fill(s);
            if (rc == PSSH_E_EOF) return PSSH_E_EOF;
            if (rc < 0 && rc != PSSH_E_AGAIN) return rc;
        }
        if (got & PSSH_W_STDIN) {
            rc = stdin_pump(s);
            if (rc != PSSH_OK && rc != PSSH_E_AGAIN) return rc;
        }
    }
    /* the channel is closed: report the status the server gave */
    pssh_flush(s);
    pssh_disconnect(s, 11, "");
    return s->have_exit ? s->exit_status : 255;
}
