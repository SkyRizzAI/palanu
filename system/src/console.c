/* The system console (pal/console.h, ADR-0010). Its own 8-bit canvas in external RAM, its own palette (the terminal colours with
 * fixed defaults, not the theme's), the shared terminal engine (pal_term) and a 6x12 Spleen font. Text from other tasks goes
 * through a locked queue and reaches the terminal on the screen loop's task, which is the only task that touches the terminal,
 * the canvas and the shell. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nema/log.h"
#include "nema/nema.h"
#include "nema/tty.h"
#include "pal/board.h"
#include "pal/console.h"
#include "desktop_int.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/display.h"
#include "pal/dpm.h"
#include "pal/font.h"
#include "pal/gfx.h"
#include "pal/perf.h"
#include "pal/svc.h"
#include "pal/term.h"
#include "pal/theme.h"
#include "pal/ui.h"
#include "pal/version.h"

#define QUEUE_BYTES 4096
#define BOOT_MIN_MS 2000u  /* the splash shows at least this long, as the earlier boot splash did */
#define BOOT_MAX_MS 5000u  /* and never waits longer than this for the stages */
#define BOOT_STAGES 6      /* start, kernel, storage, display, desktop, services (boot.c) */
#define BLINK_MS 500u
#define BAD_PIN_WAIT_MS 3000u

typedef enum { M_SPLASH, M_LOG, M_LOGIN, M_SHELL } mode_t_;

static char s_queue[QUEUE_BYTES] NEMA_EXT_BSS;

static struct {
    bool ready, front, booting, capture, full, cursor_on, login_shown, locked_seen;
    mode_t_ mode;
    uint32_t gen;
    uint32_t t0, blink_at, wait_until;
    float painted;
    nema_port_mutex_t *lock;
    size_t qhead, qlen;
    nema_owner_t owner;
    pal_canvas_t cv;
    uint16_t pal[256];
    pal_term_t *term;
    int x0, y0;
    int cursor_row; /* the row the cursor was drawn on: it is drawn again without it when the cursor moves */
    nema_tty_t *tty;
    nema_pid_t pid;
    char pin[PAL_UI_PIN_MAX + 1];
    int npin;
} C NEMA_EXT_BSS; /* the palette alone is 512 bytes: internal RAM is kept for Wi-Fi */

static const pal_font_t *font(void) { return &PAL_FONT_SPLEEN_6X12; }

static uint16_t swapped565(uint32_t rgb) {
    uint16_t v = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    return (uint16_t)((v >> 8) | (v << 8));
}

/* The xterm colours from the shared table, then fixed defaults in the slots a theme would set. */
static void palette(void) {
    pal_theme_palette(C.pal);
    C.pal[PAL_C_TERM_BG] = swapped565(0x000000);
    C.pal[PAL_C_TERM_FG] = swapped565(0xC8C8C8);
    C.pal[PAL_C_PAPER] = swapped565(0x000000);
    C.pal[PAL_C_INK] = swapped565(0xFFFFFF);
    C.pal[PAL_C_DIM] = swapped565(0x707070);
    C.pal[PAL_C_PANEL] = swapped565(0x262626);
    C.pal[PAL_C_ACCENT] = swapped565(0x3B8EEA);
    C.pal[PAL_C_ON_ACCENT] = swapped565(0xFFFFFF);
}

static void wake(void) {
    if (C.front && desk_started()) pal_desktop_invalidate(); /* before the loop runs, its first frame draws everything */
}

/* ---- the queue: any task writes, the screen loop reads ---- */

void pal_console_print(const char *text) {
    if (!C.ready || !text) return;
    nema_port_mutex_lock(C.lock);
    for (const char *p = text; *p; p++) {
        char two[2] = {*p, 0};
        size_t n = 1;
        if (*p == '\n') two[0] = '\r', two[1] = '\n', n = 2; /* the terminal needs the carriage return; a tty adds it for a shell */
        for (size_t i = 0; i < n && C.qlen < QUEUE_BYTES; i++) {
            s_queue[(C.qhead + C.qlen) % QUEUE_BYTES] = two[i];
            C.qlen++;
        }
    }
    nema_port_mutex_unlock(C.lock);
    wake();
}

void pal_console_printf(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pal_console_print(line);
}

static void drain(void) {
    char buf[128];
    for (;;) {
        size_t n = 0;
        nema_port_mutex_lock(C.lock);
        while (n < sizeof(buf) && C.qlen) {
            buf[n++] = s_queue[C.qhead];
            C.qhead = (C.qhead + 1) % QUEUE_BYTES;
            C.qlen--;
        }
        nema_port_mutex_unlock(C.lock);
        if (!n) return;
        pal_term_write(C.term, buf, n);
    }
}

/* ---- what the console shows during the boot ---- */

static void log_sink(nema_log_level_t level, const char *line, size_t len) {
    if (!C.capture || level > NEMA_LOG_WARN) return; /* the boot shows warnings and errors; `logs` has the rest */
    char out[220];
    snprintf(out, sizeof(out), "%s%.*s\x1b[0m\n", level == NEMA_LOG_ERROR ? "\x1b[31m" : "\x1b[33m", (int)len, line);
    pal_console_print(out);
}

static void status_sink(const char *id, const char *name, psys_svc_state_t was, psys_svc_state_t now, const char *why) {
    (void)id;
    if (!C.capture) return;
    switch (now) {
    case PSYS_SVC_STARTING: pal_console_printf("         Starting %s...\n", name); break;
    case PSYS_SVC_RUNNING: pal_console_printf("[\x1b[32m  OK  \x1b[0m] Started %s.\n", name); break;
    case PSYS_SVC_FAILED: pal_console_printf("[\x1b[31mFAILED\x1b[0m] Failed to start %s: %s\n", name, why); break;
    case PSYS_SVC_WAITING_MEMORY: pal_console_printf("[\x1b[33m WAIT \x1b[0m] %s: %s\n", name, why); break;
    case PSYS_SVC_STOPPED:
        if (was == PSYS_SVC_RUNNING) pal_console_printf("[\x1b[32m  OK  \x1b[0m] Stopped %s.\n", name);
        break;
    }
}

/* ---- login and shell (the equivalent of getty and login) ---- */

static void prompt(void) {
    C.npin = 0;
    pal_console_print("\npalanu login: owner\nPIN: ");
    C.login_shown = true;
}

static void shell_start(void) {
    int cols = pal_term_cols(C.term), rows = pal_term_rows(C.term);
    if (nema_tty_create(&C.tty, (uint16_t)cols, (uint16_t)rows, C.owner) != NEMA_OK) {
        pal_console_print("\x1b[31mconsole: no memory for a terminal\x1b[0m\n");
        C.tty = NULL;
        return;
    }
    nema_obj_t *io = nema_tty_proc_obj(C.tty);
    nema_spawn_t sp = {.name = "sh", .stdio = {io, io, io}, .cwd = "/home"};
    if (nema_proc_spawn(&sp, &C.pid) != NEMA_OK) {
        pal_console_print("\x1b[31mconsole: the shell did not start\x1b[0m\n");
        nema_tty_destroy(C.tty);
        C.tty = NULL;
        return;
    }
    nema_tty_set_foreground(C.tty, C.pid);
    pal_desktop_watch(nema_tty_term_obj(C.tty));
    C.mode = M_SHELL;
}

static void shell_stop(void) {
    if (!C.tty) return;
    pal_desktop_unwatch(nema_tty_term_obj(C.tty));
    nema_proc_signal(C.pid, NEMA_SIG_HUP);
    nema_tty_destroy(C.tty); /* the shell sees the end of its input and exits */
    C.tty = NULL;
}

/* The boot is over and nobody took the screen, or the console came in front for the first time since: ask for the PIN. */
static void login(void) {
    C.capture = false; /* from here the log goes to `logs`, not over the shell */
    C.mode = M_LOGIN;
    if (pal_ui_pin_is_set()) prompt();
    else {
        pal_console_print("\npalanu login: owner (no PIN set)\n");
        shell_start();
    }
}

static void on_reply(void *ctx, const char *b, size_t n) {
    (void)ctx;
    if (C.tty) nema_tty_input(C.tty, b, n);
}

static void pin_key(const pal_key_t *k, uint32_t now) {
    if ((int32_t)(now - C.wait_until) < 0) return; /* after a wrong PIN */
    if (k->sym == PAL_KEY_CHAR && k->cp >= '0' && k->cp <= '9' && C.npin < PAL_UI_PIN_MAX) {
        C.pin[C.npin++] = (char)k->cp;
        pal_console_print("*");
    } else if (k->sym == PAL_KEY_BACKSPACE && C.npin > 0) {
        C.npin--;
        pal_console_print("\b \b");
    } else if (k->sym == PAL_KEY_ENTER) {
        C.pin[C.npin] = 0;
        bool ok = pal_ui_pin_check(C.pin);
        memset(C.pin, 0, sizeof(C.pin));
        C.npin = 0;
        if (ok) {
            pal_console_print("\n");
            shell_start();
        } else {
            pal_console_print("\nLogin incorrect\n");
            C.wait_until = now + BAD_PIN_WAIT_MS;
            C.login_shown = false; /* the prompt comes back when the wait is over */
        }
    }
}

/* ---- the screen loop ---- */

static uint32_t now_ms(void) { return (uint32_t)(nema_port_time_us() / 1000u); }

bool pal_console_front(void) { return C.ready && C.front; }
uint32_t pal_console_generation(void) { return C.gen; }

static void take_screen(void) {
    C.front = true;
    C.full = true;
    C.gen++;
    pal_desktop_invalidate_all();
}

static void post_show(void *arg) {
    (void)arg;
    if (C.front) return;
    take_screen();
    if (!C.booting && C.mode != M_SHELL && !C.login_shown) login();
}

static void post_hide(void *arg) {
    (void)arg;
    if (!C.front || C.booting || !pal_desktops_active()[0]) return;
    C.front = false;
    pal_desktop_invalidate_all(); /* the session's screen is stale: everything is drawn again */
}

/* The console's state belongs to the screen loop's task: a call from another task (a shell command) is posted to it. */
static void on_loop(void (*fn)(void *arg)) {
    if (!desk_started() || desk_on_task()) fn(NULL);
    else (void)desk_post(fn, NULL);
}

void pal_console_show(void) {
    if (C.ready) on_loop(post_show);
}

void pal_console_hide(void) {
    if (C.ready) on_loop(post_hide);
}

static bool switch_key(const pal_key_t *k) {
    return k->sym == PAL_KEY_ESC && (k->mods & PAL_MOD_CTRL) && (k->mods & PAL_MOD_ALT);
}

bool pal_console_key(const pal_key_t *k) {
    if (!C.ready) return false;
    if (switch_key(k)) {
        if (C.booting) return true;
        if (!C.front) pal_console_show();
        else if (pal_desktops_active()[0]) pal_console_hide();
        else pal_console_print("\n(no desktop session is running: `desktop use ID` starts one)\n");
        return true;
    }
    if (!C.front) return false;
    uint32_t now = now_ms();
    switch (C.mode) {
    case M_SPLASH: /* any key shows the log under the splash */
        C.mode = M_LOG;
        C.full = true;
        break;
    case M_LOG: break;
    case M_LOGIN: pin_key(k, now); break;
    case M_SHELL: {
        if ((k->mods & PAL_MOD_SHIFT) && (k->sym == PAL_KEY_PGUP || k->sym == PAL_KEY_PGDN)) {
            int half = pal_term_rows(C.term) / 2;
            pal_term_scroll_view(C.term, k->sym == PAL_KEY_PGUP ? half : -half);
            break;
        }
        if (pal_term_view_offset(C.term) > 0) pal_term_scroll_view(C.term, -100000);
        char bytes[16];
        size_t n = pal_key_to_bytes(k, pal_term_app_cursor_keys(C.term), bytes, sizeof(bytes));
        if (n && C.tty) nema_tty_input(C.tty, bytes, n);
        C.cursor_on = true;
        break;
    }
    }
    pal_desktop_invalidate();
    return true;
}

static void boot_finish(void) {
    C.booting = false;
    if (pal_desktops_active()[0]) {
        pal_console_print("\nThe desktop session has the screen. Ctrl+Alt+Esc comes back here.\n");
        C.capture = false;
        C.front = false;
        pal_desktop_invalidate_all();
        return;
    }
    if (C.mode == M_SPLASH) C.mode = M_LOG, C.full = true;
    login();
}

void pal_console_pump(uint32_t now) {
    if (!C.ready) return;
    if (!C.t0) C.t0 = now ? now : 1;
    bool had = C.qlen > 0;
    drain();
    if (C.tty) {
        char buf[256];
        int n, budget = 4;
        while (budget-- > 0 && (n = nema_tty_output_read(C.tty, buf, sizeof(buf), 0)) > 0) {
            pal_term_write(C.term, buf, (size_t)n);
            had = true;
        }
        int code;
        if (nema_proc_wait(C.pid, &code, 0) == NEMA_OK) { /* the shell exited: log in again, as getty does */
            shell_stop();
            pal_console_print("\n");
            C.login_shown = false;
            login();
            had = true;
        }
    }
    /* Locking the device logs the console out when a PIN is set: its shell is the owner's too. */
    bool locked = pal_dpm_locked();
    if (locked && !C.locked_seen && C.mode == M_SHELL && pal_ui_pin_is_set()) {
        shell_stop();
        pal_console_print("\n[the device locked: logged out]\n");
        C.login_shown = false;
        C.mode = M_LOGIN;
        if (C.front) prompt();
        had = true;
    }
    C.locked_seen = locked;
    if (C.mode == M_LOGIN && !C.login_shown && C.front && (int32_t)(now - C.wait_until) >= 0 && !C.booting) {
        prompt();
        had = true;
    }
    if (C.booting) {
        uint32_t el = now - C.t0;
        if ((el >= BOOT_MIN_MS && pal_boot_stage_count() >= BOOT_STAGES) || el >= BOOT_MAX_MS) boot_finish();
        else if (C.mode == M_SPLASH && C.front) had = true; /* the progress bar moves */
    }
    if (C.front && C.mode >= M_LOGIN && now - C.blink_at >= BLINK_MS) {
        C.blink_at = now;
        C.cursor_on = !C.cursor_on;
        int cx, cy;
        bool vis;
        pal_term_cursor(C.term, &cx, &cy, &vis);
        pal_term_mark_row(C.term, cy);
        had = true;
    }
    if (had) wake();
}

int32_t pal_console_next_ms(uint32_t now) {
    if (!C.ready || !C.front) return C.ready && C.booting ? 100 : -1;
    if (C.booting) return C.mode == M_SPLASH ? 50 : 100;
    if (C.mode == M_LOGIN && !C.login_shown) {
        int32_t d = (int32_t)(C.wait_until - now);
        return d < 0 ? 0 : d;
    }
    if (C.mode >= M_LOGIN) {
        int32_t d = (int32_t)(BLINK_MS - (now - C.blink_at));
        return d < 0 ? 0 : d;
    }
    return -1;
}

/* A plain splash, the system's own: the name, a progress bar, and how to see the log. */
static void splash(uint32_t now) {
    pal_canvas_t *c = &C.cv;
    uint32_t el = now - C.t0;
    float t = el >= BOOT_MIN_MS ? 1.0f : (float)el / (float)BOOT_MIN_MS;
    float s = (float)pal_boot_stage_count() / (float)BOOT_STAGES;
    float p = t < s ? t : s;
    if (p > 1.0f) p = 1.0f;
    if (!C.full && p == C.painted) return;
    const pal_font_t *big = &PAL_FONT_SPLEEN_8X16, *small = &PAL_FONT_SPLEEN_5X8;
    const char *name = "PalanuOS", *hint = "any key: boot log";
    int cy = c->h / 2;
    if (C.full) {
        pal_clear(c, PAL_C_PAPER);
        pal_text(c, big, (c->w - pal_text_w(big, name)) / 2, cy - 22, name, PAL_C_INK, PAL_C_PAPER);
        pal_text(c, small, (c->w - pal_text_w(small, hint)) / 2, c->h - 14, hint, PAL_C_DIM, PAL_C_PAPER);
    }
    int bw = 120, bx = (c->w - bw) / 2;
    pal_fill(c, bx, cy + 4, bw, 3, PAL_C_PANEL);
    pal_fill(c, bx, cy + 4, (int)((float)bw * p), 3, PAL_C_INK);
    C.painted = p;
}

void pal_console_render(bool full) {
    if (!C.ready || !C.front || !pal_dpm_display_on()) return;
    if (full) C.full = true;
    uint32_t now = now_ms();
    if (C.mode == M_SPLASH) splash(now);
    else {
        if (C.full) {
            pal_clear(&C.cv, PAL_C_TERM_BG);
            pal_term_mark_all(C.term);
        }
        int cx, cy;
        bool vis;
        pal_term_cursor(C.term, &cx, &cy, &vis);
        if (cy != C.cursor_row) pal_term_mark_row(C.term, C.cursor_row);
        C.cursor_row = cy;
        pal_term_draw(C.term, &C.cv, font(), C.x0, C.y0, C.cursor_on && C.mode >= M_LOGIN);
    }
    C.full = false;
    pal_rect_t r;
    if (pal_take_dirty(&C.cv, &r)) pal_display_present_indexed(C.cv.px, C.cv.stride, r, C.pal);
}

void pal_console_init(bool quiet) {
    if (C.ready || !pal_display_canvas()) return;
    pal_canvas_t *screen = pal_display_canvas();
    int w = screen->w, h = screen->h;
    C.owner = nema_owner_register("syscon"); /* "console" is the serial console's */
    void *px = nema_alloc_ex(NEMA_MEM_EXTERNAL, C.owner, (size_t)w * (size_t)h, 64, NEMA_ALLOC_ZERO);
    if (!px) px = nema_alloc_ex(NEMA_MEM_ANY, C.owner, (size_t)w * (size_t)h, 64, NEMA_ALLOC_ZERO);
    if (!px) return;
    const pal_font_t *f = font();
    int cols = w / f->w, rows = h / f->h;
    if (cols > PAL_TERM_MAX_COLS) cols = PAL_TERM_MAX_COLS;
    if (rows > PAL_TERM_MAX_ROWS) rows = PAL_TERM_MAX_ROWS;
    C.term = pal_term_create(cols, rows, 200, C.owner);
    if (!C.term) {
        nema_free(px);
        return;
    }
    pal_canvas_init(&C.cv, px, w, h);
    C.x0 = (w - cols * f->w) / 2;
    C.y0 = (h - rows * f->h) / 2;
    pal_term_set_reply(C.term, on_reply, NULL);
    palette();
    C.lock = nema_port_mutex_create();
    C.mode = quiet ? M_SPLASH : M_LOG;
    C.booting = C.capture = C.front = C.full = true;
    C.painted = -1.0f;
    C.cursor_on = true;
    C.gen = 1;
    C.ready = true;
    nema_log_set_sink(log_sink);
    psys_svc_set_status_sink(status_sink);
    const pal_board_desc_t *b = pal_board();
    pal_console_printf("PalanuOS %s on %s (%s)\n\n", PAL_OS_VERSION, b && b->name ? b->name : "?", b && b->chip ? b->chip : "?");
}
