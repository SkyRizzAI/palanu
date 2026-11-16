#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/legend.h"
#include "pal/term.h"
#include "pal/vk.h"
#include "pal/theme.h"

/* The Terminal app: tabs of shell sessions. Each session is a TTY, an emulator, and a shell process, so
 * the same shell works on the screen, on a UART, and (later) through Palanu Link. */

#define MAX_SESS 4

typedef struct {
    nema_tty_t *tty;
    pal_term_t *term;
    nema_pid_t pid;
    bool used;
    uint32_t bells;
} sess_t;

static struct {
    sess_t s[MAX_SESS];
    int cur;
    int font; /* 0 = 5x8, 1 = 6x12, 2 = 8x16 */
    bool cursor_on;
    nema_owner_t owner;
    bool ready;
    pal_rect_t area;
} T = {.font = 1, .cursor_on = true};

/* Legends a program in the session in front published (pal_legend_show, api/palanu/legend.h). Two buffers alternate so
 * pal_legend_set sees a new array whenever the text changes. A text that no longer parses shows nothing. */
static struct {
    bool valid;   /* sess and gen describe a read */
    int sess;     /* the session the legends came from */
    uint32_t gen; /* its tty's legend counter at that read */
    int buf;
    char text[2][NEMA_TTY_LEGEND_MAX];
    pal_legend_item_t items[2][PAL_LEGEND_MAX];
} L;

static const pal_font_t *font_of(int f) { return f == 0 ? &PAL_FONT_SPLEEN_5X8 : f == 2 ? &PAL_FONT_SPLEEN_8X16 : &PAL_FONT_SPLEEN_6X12; }

static void geometry(pal_view_t *v, int *cols, int *rows) {
    pal_rect_t a = pal_desktop_area(v);
    const pal_font_t *f = font_of(T.font);
    *cols = a.w / f->w;
    *rows = a.h / f->h;
    if (*cols > PAL_TERM_MAX_COLS) *cols = PAL_TERM_MAX_COLS;
    if (*rows > PAL_TERM_MAX_ROWS) *rows = PAL_TERM_MAX_ROWS;
}

static void on_reply(void *ctx, const char *b, size_t n) {
    sess_t *s = (sess_t *)ctx;
    nema_tty_input(s->tty, b, n); /* the terminal answers the program's queries as if typed */
}

static int session_start(pal_view_t *v, int idx) {
    sess_t *s = &T.s[idx];
    int cols, rows;
    geometry(v, &cols, &rows);
    if (!T.owner) T.owner = nema_owner_register("terminal");
    if (nema_tty_create(&s->tty, (uint16_t)cols, (uint16_t)rows, T.owner) != NEMA_OK) return -1;
    s->term = pal_term_create(cols, rows, 200, T.owner);
    if (!s->term) {
        nema_tty_destroy(s->tty);
        return -1;
    }
    pal_term_set_reply(s->term, on_reply, s);
    nema_obj_t *io = nema_tty_proc_obj(s->tty);
    nema_spawn_t sp = {.name = "sh", .stdio = {io, io, io}, .cwd = "/home"};
    if (nema_proc_spawn(&sp, &s->pid) != NEMA_OK) {
        pal_term_destroy(s->term);
        nema_tty_destroy(s->tty);
        return -1;
    }
    nema_tty_set_foreground(s->tty, s->pid);
    pal_desktop_watch(nema_tty_term_obj(s->tty));
    s->used = true;
    s->bells = 0;
    return 0;
}

static void session_close(int idx) {
    sess_t *s = &T.s[idx];
    if (!s->used) return;
    pal_desktop_unwatch(nema_tty_term_obj(s->tty));
    nema_proc_signal(s->pid, NEMA_SIG_HUP);
    nema_tty_destroy(s->tty); /* the shell sees end of input and exits by itself */
    pal_term_destroy(s->term);
    memset(s, 0, sizeof(*s));
    if (L.sess == idx) L.valid = false; /* a new tty in this slot starts its counter again */
}

static int count(void) {
    int n = 0;
    for (int i = 0; i < MAX_SESS; i++) n += T.s[i].used ? 1 : 0;
    return n;
}

static void resize_all(pal_view_t *v) {
    int cols, rows;
    geometry(v, &cols, &rows);
    for (int i = 0; i < MAX_SESS; i++) {
        if (!T.s[i].used) continue;
        pal_term_resize(T.s[i].term, cols, rows);
        nema_tty_set_size(T.s[i].tty, (uint16_t)cols, (uint16_t)rows);
    }
}

/* A command from the launcher (pal_terminal_run) waits here until the session it belongs to exists. */
static char s_run[96];
static bool s_run_set;

static void type_pending(void) {
    if (!s_run_set || !T.ready || !T.s[T.cur].used) return;
    size_t n = strlen(s_run);
    s_run[n++] = '\r'; /* Enter, as the keyboard sends it */
    nema_tty_input(T.s[T.cur].tty, s_run, n);
    s_run_set = false;
}

/* The on-screen keyboard shows the line the active tab is editing (vk.h). */
static int vk_text(void *ctx, char *out, size_t cap, size_t *cursor) {
    (void)ctx;
    if (!T.ready || !T.s[T.cur].used) return -1;
    return nema_tty_get_edit(T.s[T.cur].tty, out, cap, cursor);
}
static pal_vk_source_t s_vk_src = {vk_text, NULL};

static void enter(pal_view_t *v) {
    static bool vk_registered;
    if (!vk_registered) {
        vk_registered = true;
        (void)pal_vk_register_source("terminal", &s_vk_src);
    }
    if (!T.ready) {
        T.ready = true;
        session_start(v, 0);
    }
    resize_all(v);
    if (T.s[T.cur].used) pal_term_mark_all(T.s[T.cur].term);
    type_pending();
}

/* Opens the Terminal and types cmdline into its shell, as if the user had typed it and pressed Enter. The launcher uses it
 * for installed tty apps. */
void pal_terminal_run(const char *cmdline) {
    nema_strlcpy(s_run, cmdline, sizeof(s_run));
    s_run_set = true;
    pal_desktop_open("terminal");
    type_pending(); /* the Terminal was already in front: enter() did not run */
}


static void legends_sync(pal_view_t *v) {
    sess_t *s = &T.s[T.cur];
    if (!s->used) return;
    char raw[NEMA_TTY_LEGEND_MAX];
    uint32_t gen = nema_tty_get_legend(s->tty, raw, sizeof(raw));
    if (L.valid && L.sess == T.cur && L.gen == gen) return;
    L.valid = true;
    L.sess = T.cur;
    L.gen = gen;
    L.buf ^= 1;
    int n = pal_legend_parse(raw, L.items[L.buf], PAL_LEGEND_MAX, L.text[L.buf], sizeof(L.text[0]));
    pal_legend_set(n > 0 ? L.items[L.buf] : NULL, n > 0 ? n : 0);
    resize_all(v); /* the bar took rows from the terminal or gave them back */
}

/* The area under the bars changes with the UI scale (the bars are logical rows): the terminal follows. */
static void area_sync(pal_view_t *v) {
    pal_rect_t a = pal_desktop_area(v);
    if (a.w == T.area.w && a.h == T.area.h) return;
    T.area = a;
    resize_all(v);
}

/* Output pump: runs for every view every loop, so background sessions keep draining their TTY. */
static void pump(pal_view_t *v) {
    if (!T.ready) return;
    bool front = strcmp(pal_desktop_front_id(), "terminal") == 0;
    char buf[256];
    for (int i = 0; i < MAX_SESS; i++) {
        sess_t *s = &T.s[i];
        if (!s->used) continue;
        int budget = 4;
        int n;
        while (budget-- > 0 && (n = nema_tty_output_read(s->tty, buf, sizeof(buf), 0)) > 0) {
            pal_term_write(s->term, buf, (size_t)n);
            if (front && i == T.cur) pal_desktop_invalidate();
        }
        uint32_t bells = pal_term_bell_count(s->term);
        if (bells != s->bells) {
            s->bells = bells;
            pal_beep(60);
        }
        int code;
        if (nema_proc_wait(s->pid, &code, 0) == NEMA_OK) { /* the shell exited: replace it */
            if (count() > 1) {
                session_close(i);
                if (T.cur == i) {
                    for (int j = 0; j < MAX_SESS; j++)
                        if (T.s[j].used) T.cur = j;
                }
            } else {
                session_close(i);
                session_start(v, i);
            }
            pal_desktop_invalidate_all();
        }
    }
    legends_sync(v);
    area_sync(v);
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    sess_t *s = &T.s[T.cur];
    if (!s->used) return;
    const pal_font_t *f = font_of(T.font);
    if (full) {
        pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_TERM_BG);
        pal_term_mark_all(s->term);
    }
    pal_term_draw(s->term, c, f, a.x, a.y, T.cursor_on);
    (void)v;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    T.cursor_on = !T.cursor_on;
    sess_t *s = &T.s[T.cur];
    if (s->used) {
        int cx, cy;
        bool vis;
        pal_term_cursor(s->term, &cx, &cy, &vis);
        pal_term_mark_row(s->term, cy);
        pal_desktop_invalidate();
    }
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    sess_t *s = &T.s[T.cur];
    if ((k->mods & PAL_MOD_ALT) && k->sym == PAL_KEY_CHAR) { /* tab management */
        if (k->cp == 'n' || k->cp == 'N') {
            for (int i = 0; i < MAX_SESS; i++) {
                if (!T.s[i].used && session_start(v, i) == 0) {
                    T.cur = i;
                    pal_desktop_invalidate_all();
                    break;
                }
            }
            return true;
        }
        if (k->cp == 'w' || k->cp == 'W') {
            if (count() > 1) {
                session_close(T.cur);
                for (int j = 0; j < MAX_SESS; j++)
                    if (T.s[j].used) T.cur = j;
                pal_desktop_invalidate_all();
            }
            return true;
        }
    }
    if ((k->mods & PAL_MOD_ALT) && k->sym == PAL_KEY_TAB) {
        for (int j = 1; j <= MAX_SESS; j++) {
            int n = (T.cur + j) % MAX_SESS;
            if (T.s[n].used) {
                T.cur = n;
                break;
            }
        }
        pal_desktop_invalidate_all();
        return true;
    }
    if (!s->used) return true;
    if ((k->mods & PAL_MOD_SHIFT) && (k->sym == PAL_KEY_PGUP || k->sym == PAL_KEY_UP)) {
        pal_term_scroll_view(s->term, k->sym == PAL_KEY_PGUP ? pal_term_rows(s->term) / 2 : 1);
        return true;
    }
    if ((k->mods & PAL_MOD_SHIFT) && (k->sym == PAL_KEY_PGDN || k->sym == PAL_KEY_DOWN)) {
        pal_term_scroll_view(s->term, k->sym == PAL_KEY_PGDN ? -pal_term_rows(s->term) / 2 : -1);
        return true;
    }
    if (pal_term_view_offset(s->term) > 0) { /* any other key returns to the live screen */
        pal_term_scroll_view(s->term, -100000);
    }
    char bytes[16];
    size_t n = pal_key_to_bytes(k, pal_term_app_cursor_keys(s->term), bytes, sizeof(bytes));
    if (n) nema_tty_input(s->tty, bytes, n);
    T.cursor_on = true;
    return true;
}

/* Font size is changed from Settings. */
void pal_terminal_set_font(int f);
int pal_terminal_font(void);
void pal_terminal_set_font(int f) {
    T.font = f < 0 ? 0 : f > 2 ? 2 : f;
    if (T.ready) resize_all(&pal_terminal_view);
}
int pal_terminal_font(void) { return T.font; }

pal_view_t pal_terminal_view = {.id = "terminal", .title = "Terminal", .enter = enter, .draw = draw, .key = key,
                                 .pump = pump, .tick = tick, .tick_ms = 500};
