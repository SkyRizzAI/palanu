/* Palanu Link, device side. See docs/PLP.md for the wire format.
 *
 * One pal_link_t serves one connection. Receiving happens in whoever calls pal_link_feed (the transport's
 * task); a worker task per link pumps the remote shell's output and mirrors the screen. Frames are built in a
 * single per-link buffer under the transmit lock, so at most one frame is in flight and memory is bounded. */
#include "link_auth.h"
#include "link_clients.h"
#include "link_int.h"
#include "settings_api.h"

#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/version.h"
#include "pal/board.h"
#include "pal/sysinfo.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/input.h"
#include "pal/ota.h"
#include "pal/papp.h"

#define MAGIC 0xABu
#define MAX_PAYLOAD 1024u

enum { CH_CTL = 0, CH_TTY = 1, CH_SCREEN = 2, CH_INPUT = 3, CH_FS = 4, CH_OTA = 5 };
enum { CTL_HELLO = 1, CTL_INFO = 2, CTL_PING = 3, CTL_PONG = 4, ERR = 0x7F };
enum { TTY_OPEN = 1, TTY_DATA = 2, TTY_RESIZE = 3, TTY_CLOSE = 4 };
enum { SCR_SUBSCRIBE = 1, SCR_PALETTE = 2, SCR_RECT = 3, SCR_REFRESH = 4, SCR_SIZE = 5 };
enum { OTA_BEGIN = 1, OTA_DATA = 2, OTA_END = 3, OTA_OK = 4 };
enum { FS_LIST = 1, FS_ENTRY = 2, FS_END = 3, FS_READ = 4, FS_DATA = 5, FS_WRITE = 6, FS_OK = 7 };

enum { RX_IDLE, RX_HEAD, RX_BODY, RX_CRC };

/* struct pal_link: link_int.h */

static pal_link_t *s_screen_link;

static uint16_t crc16(uint16_t crc, const uint8_t *p, size_t n) {
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---- transmit ---- */

/* The caller holds txlock and has put the payload at txbuf + 5. */
static int send_built(pal_link_t *l, uint8_t chan, uint8_t type, size_t len) {
    uint8_t *b = l->txbuf;
    b[0] = MAGIC;
    b[1] = chan;
    b[2] = type;
    b[3] = (uint8_t)len;
    b[4] = (uint8_t)(len >> 8); /* the payload is already at b + 5 */
    uint16_t c = crc16(0xFFFF, b + 1, 4 + len);
    b[5 + len] = (uint8_t)c;
    b[6 + len] = (uint8_t)(c >> 8);
    return l->io.tx(l->io.ctx, b, 7 + len);
}

static int send_frame(pal_link_t *l, uint8_t chan, uint8_t type, const void *payload, size_t len) {
    if (len > MAX_PAYLOAD) return -1;
    nema_port_mutex_lock(l->txlock);
    if (len) memcpy(l->txbuf + 5, payload, len);
    int r = send_built(l, chan, type, len);
    nema_port_mutex_unlock(l->txlock);
    return r;
}

static void send_err(pal_link_t *l, uint8_t chan, int8_t code, const char *msg) {
    uint8_t b[64];
    b[0] = (uint8_t)code;
    size_t n = strlen(msg);
    if (n > 60) n = 60;
    memcpy(b + 1, msg, n);
    send_frame(l, chan, ERR, b, n + 1);
}

static void send_fs_err(pal_link_t *l, uint8_t id, int code) {
    uint8_t b[3] = {id, (uint8_t)(int8_t)code, 0};
    send_frame(l, CH_FS, ERR, b, 2);
}

/* ---- ctl ---- */

/* INFO: the full set on a trusted link. Untrusted, only what identifies the device: name, board, version, and the id
 * (random, not a secret; the client looks its token up by it). */
static void send_info(pal_link_t *l) {
    const pal_board_desc_t *bo = pal_board_info();
    const pal_ota_ops_t *po = pal_ota();
    char name[32] = "PalanuOS", id[17];
    if (psys_settings_get("device.name", name, sizeof(name)) < 0) nema_strlcpy(name, "PalanuOS", sizeof(name));
    pal_link_device_id(id);
    char j[320];
    int n;
    if (!l->trusted) {
        n = snprintf(j, sizeof(j), "{\"os\":\"PalanuOS\",\"ver\":\"%s\",\"board\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"trusted\":false}",
                     PAL_OS_VERSION, bo && bo->name ? bo->name : "?", name, id);
    } else {
        n = snprintf(j, sizeof(j),
                     "{\"os\":\"PalanuOS\",\"ver\":\"%s\",\"board\":\"%s\",\"chip\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"trusted\":true,"
                     "\"uptime\":%lu,\"mem_free\":%lu,\"psram_free\":%lu,\"slot\":\"%s\",\"build\":\"%s\",\"ota\":%s}",
                     PAL_OS_VERSION, bo && bo->name ? bo->name : "?", bo && bo->chip ? bo->chip : "?", name, id,
                     (unsigned long)(pal_sys_uptime_us() / 1000000u),
                     (unsigned long)pal_sys_mem_free(PAL_MEM_INTERNAL),
                     (unsigned long)pal_sys_mem_free(PAL_MEM_EXTERNAL), po && po->running ? po->running() : "",
                     po && po->version ? po->version() : "", po ? "true" : "false");
    }
    send_frame(l, CH_CTL, CTL_INFO, j, (size_t)n);
}

/* ---- remote shell ---- */

static void tty_close(pal_link_t *l) {
    nema_port_mutex_lock(l->tlock);
    if (!l->tty) {
        nema_port_mutex_unlock(l->tlock);
        return;
    }
    if (l->pid) nema_proc_signal_tree(l->pid, NEMA_SIG_HUP | NEMA_SIG_KILL); /* the shell and every job it started */
    nema_tty_destroy(l->tty);
    if (l->pid) {
        int code;
        for (int i = 0; i < 50 && nema_proc_wait(l->pid, &code, 20) != NEMA_OK; i++) {}
    }
    l->tty = NULL;
    l->pid = 0;
    nema_port_mutex_unlock(l->tlock);
}

static void tty_open(pal_link_t *l, uint16_t cols, uint16_t rows) {
    tty_close(l);
    nema_port_mutex_lock(l->tlock);
    if (cols < 10) cols = 80;
    if (rows < 2) rows = 24;
    if (nema_tty_create(&l->tty, cols, rows, l->owner) != NEMA_OK) {
        l->tty = NULL;
        nema_port_mutex_unlock(l->tlock);
        send_err(l, CH_TTY, (int8_t)NEMA_ERR_NOMEM, "no memory for a terminal");
        return;
    }
    nema_obj_t *io = nema_tty_proc_obj(l->tty);
    nema_spawn_t sp = {.name = "sh", .stdio = {io, io, io}, .cwd = "/home"};
    if (nema_proc_spawn(&sp, &l->pid) != NEMA_OK) {
        nema_tty_destroy(l->tty);
        l->tty = NULL;
        nema_port_mutex_unlock(l->tlock);
        send_err(l, CH_TTY, (int8_t)NEMA_ERR_NOMEM, "cannot start the shell");
        return;
    }
    nema_tty_set_foreground(l->tty, l->pid);
    nema_port_mutex_unlock(l->tlock);
}

/* ---- screen ---- */

static void tap(void *ctx, pal_rect_t r) {
    pal_link_t *l = ctx;
    nema_port_mutex_lock(l->dlock);
    if (l->dirty.w == 0) {
        l->dirty = r;
    } else {
        int x1 = l->dirty.x + l->dirty.w, y1 = l->dirty.y + l->dirty.h;
        if (r.x + r.w > x1) x1 = r.x + r.w;
        if (r.y + r.h > y1) y1 = r.y + r.h;
        if (r.x < l->dirty.x) l->dirty.x = r.x;
        if (r.y < l->dirty.y) l->dirty.y = r.y;
        l->dirty.w = x1 - l->dirty.x;
        l->dirty.h = y1 - l->dirty.y;
    }
    nema_port_mutex_unlock(l->dlock);
}

static void screen_all(pal_link_t *l) {
    pal_canvas_t *c = pal_display_canvas();
    nema_port_mutex_lock(l->dlock);
    l->dirty = (pal_rect_t){0, 0, c->w, c->h};
    l->send_palette = true;
    nema_port_mutex_unlock(l->dlock);
}

static void screen_subscribe(pal_link_t *l, bool on) {
    if (on && !l->screen_on) {
        s_screen_link = l;
        pal_display_set_tap(tap, l);
        l->screen_on = true;
        pal_canvas_t *c = pal_display_canvas();
        uint8_t sz[4] = {(uint8_t)c->w, (uint8_t)(c->w >> 8), (uint8_t)c->h, (uint8_t)(c->h >> 8)};
        send_frame(l, CH_SCREEN, SCR_SIZE, sz, 4);
        screen_all(l);
    } else if (!on && l->screen_on) {
        l->screen_on = false;
        if (s_screen_link == l) {
            pal_display_set_tap(NULL, NULL);
            s_screen_link = NULL;
        }
    }
}

static void send_palette(pal_link_t *l) {
    const uint16_t *pal = pal_display_palette();
    for (int first = 0; first < 256; first += 128) {
        uint8_t b[4 + 256];
        b[0] = (uint8_t)first;
        b[1] = (uint8_t)(first >> 8);
        b[2] = 128;
        b[3] = 0;
        for (int i = 0; i < 128; i++) {
            uint16_t v = pal[first + i]; /* the display palette is byte-swapped for the panel's bus: undo that */
            b[4 + 2 * i] = (uint8_t)(v >> 8);
            b[5 + 2 * i] = (uint8_t)v;
        }
        send_frame(l, CH_SCREEN, SCR_PALETTE, b, sizeof(b));
    }
}

/* PackBits one row; returns the encoded size. dst needs w + w/128 + 2 bytes. */
static size_t packbits(const uint8_t *src, int w, uint8_t *dst) {
    size_t o = 0;
    int i = 0;
    while (i < w) {
        int run = 1;
        while (i + run < w && run < 128 && src[i + run] == src[i]) run++;
        if (run >= 3) {
            dst[o++] = (uint8_t)(1 - run);
            dst[o++] = src[i];
            i += run;
        } else {
            int s = i, n = 0;
            while (i < w && n < 128) {
                if (i + 2 < w && src[i] == src[i + 1] && src[i] == src[i + 2]) break;
                i++;
                n++;
            }
            if (n == 0) {
                n = 1;
                i++;
            }
            dst[o++] = (uint8_t)(n - 1);
            memcpy(dst + o, src + s, (size_t)n);
            o += (size_t)n;
        }
    }
    return o;
}

static void send_rect(pal_link_t *l, pal_rect_t r) {
    pal_canvas_t *c = pal_display_canvas();
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x + r.w > c->w) r.w = c->w - r.x;
    if (r.y + r.h > c->h) r.h = c->h - r.y;
    if (r.w <= 0 || r.h <= 0 || r.w > 480) return;
    uint8_t row[480 + 8];
    int y = r.y, end = r.y + r.h;
    while (y < end && !l->dying) {
        nema_port_mutex_lock(l->txlock);
        uint8_t *p = l->txbuf + 5;
        size_t used = 8;
        int rows = 0;
        while (y + rows < end) {
            size_t n = packbits(c->px + (size_t)(y + rows) * (size_t)c->stride + (size_t)r.x, r.w, row);
            if (used + 2 + n > MAX_PAYLOAD) break;
            p[used++] = (uint8_t)n;
            p[used++] = (uint8_t)(n >> 8);
            memcpy(p + used, row, n);
            used += n;
            rows++;
        }
        p[0] = (uint8_t)r.x; p[1] = (uint8_t)(r.x >> 8);
        p[2] = (uint8_t)y;   p[3] = (uint8_t)(y >> 8);
        p[4] = (uint8_t)r.w; p[5] = (uint8_t)(r.w >> 8);
        p[6] = (uint8_t)rows; p[7] = (uint8_t)(rows >> 8);
        int rc = rows ? send_built(l, CH_SCREEN, SCR_RECT, used) : -1;
        nema_port_mutex_unlock(l->txlock);
        if (rc < 0) return;
        y += rows;
    }
}

/* ---- file transfer ---- */

static void fs_list(pal_link_t *l, uint8_t id, const char *path) {
    nema_dirent_t e;
    for (size_t i = 0;; i++) {
        nema_err_t r = nema_fs_readdir(path, i, &e);
        if (r == NEMA_ERR_NOENT && i > 0) break;
        if (r != NEMA_OK) {
            if (i == 0) {
                nema_stat_t st;
                if (nema_fs_stat(path, &st) != NEMA_OK) {
                    send_fs_err(l, id, r);
                    return;
                }
            }
            break;
        }
        uint8_t b[8 + NEMA_NAME_MAX];
        size_t n = strlen(e.name);
        b[0] = id;
        b[1] = e.type;
        memcpy(b + 2, &e.size, 4);
        memcpy(b + 6, e.name, n);
        send_frame(l, CH_FS, FS_ENTRY, b, 6 + n);
    }
    send_frame(l, CH_FS, FS_END, &id, 1);
}

static void fs_read(pal_link_t *l, uint8_t id, const char *path) {
    nema_obj_t *f;
    nema_err_t r = nema_fs_open(path, NEMA_O_READ, &f);
    if (r != NEMA_OK) {
        send_fs_err(l, id, r);
        return;
    }
    uint8_t b[1 + 512];
    b[0] = id;
    for (;;) {
        int n = nema_obj_read(f, b + 1, 512, 1000);
        if (n < 0) {
            send_fs_err(l, id, n);
            break;
        }
        if (n == 0) {
            send_frame(l, CH_FS, FS_END, &id, 1);
            break;
        }
        if (send_frame(l, CH_FS, FS_DATA, b, 1 + (size_t)n) < 0) break;
    }
    nema_obj_unref(f);
}

static void fs_write_open(pal_link_t *l, uint8_t id, const char *path) {
    if (l->wfile) nema_obj_unref(l->wfile);
    l->wfile = NULL;
    nema_strlcpy(l->wpath, path, sizeof(l->wpath));
    nema_err_t r = nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &l->wfile);
    if (r != NEMA_OK) {
        l->wfile = NULL;
        send_fs_err(l, id, r);
        return;
    }
    send_frame(l, CH_FS, FS_OK, &id, 1);
}

static void handle_fs(pal_link_t *l, uint8_t type, const uint8_t *p, size_t n) {
    if (n < 1) return;
    uint8_t id = p[0];
    char path[160];
    if (type == FS_LIST || type == FS_READ || type == FS_WRITE) {
        size_t pl = n - 1 < sizeof(path) - 1 ? n - 1 : sizeof(path) - 1;
        memcpy(path, p + 1, pl);
        path[pl] = 0;
    }
    switch (type) {
    case FS_LIST: fs_list(l, id, path); break;
    case FS_READ: fs_read(l, id, path); break;
    case FS_WRITE: fs_write_open(l, id, path); break;
    case FS_DATA:
        if (!l->wfile) {
            send_fs_err(l, id, NEMA_ERR_STATE);
        } else if (nema_obj_write(l->wfile, p + 1, n - 1, 2000) != (int)(n - 1)) {
            nema_obj_unref(l->wfile);
            l->wfile = NULL;
            send_fs_err(l, id, NEMA_ERR_NOSPC);
        } else {
            send_frame(l, CH_FS, FS_OK, &id, 1);
        }
        break;
    case FS_END:
        if (l->wfile) {
            nema_obj_unref(l->wfile);
            l->wfile = NULL;
            size_t pn = strlen(l->wpath);
            if (strncmp(l->wpath, "/apps/", 6) == 0 && pn > 5 && strcmp(l->wpath + pn - 5, ".papp") == 0) pal_papp_scan();
        }
        send_frame(l, CH_FS, FS_OK, &id, 1);
        break;
    default: send_fs_err(l, id, NEMA_ERR_UNSUPPORTED); break;
    }
}

/* ---- firmware update ---- */

static void ota_reply(pal_link_t *l, uint8_t type, int code) {
    uint8_t b[1] = {(uint8_t)(int8_t)code};
    send_frame(l, CH_OTA, type, b, 1);
}

static void reboot_later(void *arg) {
    nema_sleep_ms(800); /* let the acknowledgement leave the wire first */
    pal_reboot();
}

static void handle_ota(pal_link_t *l, uint8_t type, const uint8_t *p, size_t n) {
    const pal_ota_ops_t *o = pal_ota();
    if (!o) {
        ota_reply(l, ERR, NEMA_ERR_UNSUPPORTED);
        return;
    }
    nema_err_t r = NEMA_OK;
    if (type == OTA_BEGIN && n >= 4) {
        if (l->ota_open) o->abort();
        uint32_t size = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        r = o->begin(size);
        l->ota_open = r == NEMA_OK;
    } else if (type == OTA_DATA) {
        r = l->ota_open ? o->write(p, n) : NEMA_ERR_STATE;
        if (r != NEMA_OK && l->ota_open) {
            o->abort();
            l->ota_open = false;
        }
    } else if (type == OTA_END) {
        r = l->ota_open ? o->end() : NEMA_ERR_STATE;
        l->ota_open = false;
        if (r == NEMA_OK) {
            ota_reply(l, OTA_OK, 0);
            nema_task_desc_t td = {.name = "reboot", .fn = reboot_later, .stack = 3072, .prio = 3, .core = -1,
                                   .role = NEMA_TASK_SYSTEM, .owner = l->owner};
            nema_task_create(&td);
            return;
        }
    } else {
        r = NEMA_ERR_UNSUPPORTED;
    }
    ota_reply(l, r == NEMA_OK ? OTA_OK : ERR, r);
}

/* ---- sessions ---- */

void link_session_end(pal_link_t *l) {
    screen_subscribe(l, false);
    tty_close(l);
    if (l->ota_open && pal_ota()) pal_ota()->abort();
    l->ota_open = false;
    if (l->wfile) {
        nema_obj_unref(l->wfile);
        l->wfile = NULL;
    }
}

/* ---- dispatch ---- */

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Trust (docs/PLP.md, "Trust"): an untrusted link takes these ctl types and no other frame. */
static bool open_to_untrusted(uint8_t chan, uint8_t type) {
    if (chan != CH_CTL) return false;
    return type == CTL_HELLO || type == CTL_PING || type == LA_TYPE_PAIR_REQUEST || type == LA_TYPE_PAIR_PROOF ||
           type == LA_TYPE_AUTH_HELLO || type == LA_TYPE_AUTH_RESPONSE;
}

/* A refused frame gets an error on its own channel, with NEMA_ERR_AUTH (E_AUTH). */
static void refuse(pal_link_t *l, uint8_t chan, const uint8_t *p, size_t n) {
    if (chan == CH_FS) {
        if (n >= 1) send_fs_err(l, p[0], NEMA_ERR_AUTH);
    } else if (chan == CH_OTA) {
        ota_reply(l, ERR, NEMA_ERR_AUTH);
    } else {
        send_err(l, chan, (int8_t)NEMA_ERR_AUTH, "not authenticated: run `palanu pair`");
    }
}

static void handle_frame(pal_link_t *l, uint8_t chan, uint8_t type, const uint8_t *p, size_t n) {
    if (!l->trusted && !open_to_untrusted(chan, type)) {
        refuse(l, chan, p, n);
        return;
    }
    switch (chan) {
    case CH_CTL:
        if (type == CTL_HELLO) {
            /* a new client: a persistent transport (the UART) may still carry the previous client's session */
            link_session_end(l);
            l->hello = true;
            send_info(l);
        } else if (type == CTL_PING) {
            send_frame(l, CH_CTL, CTL_PONG, p, n);
        } else if (open_to_untrusted(chan, type)) {
            la_reply_t rep;
            la_handle(&l->auth, l->tr, type, p, n, &rep);
            if (rep.send) send_frame(l, CH_CTL, rep.type, rep.payload, rep.len);
            if (rep.trust) {
                l->kill = false;
                l->trusted = true;
            }
        }
        break;
    case CH_TTY:
        if (type == TTY_OPEN && n >= 4) tty_open(l, u16(p), u16(p + 2));
        else if (type == TTY_DATA && l->tty) nema_tty_input(l->tty, p, n);
        else if (type == TTY_RESIZE && l->tty && n >= 4) nema_tty_set_size(l->tty, u16(p), u16(p + 2));
        else if (type == TTY_CLOSE) tty_close(l);
        break;
    case CH_SCREEN:
        if (type == SCR_SUBSCRIBE && n >= 1) screen_subscribe(l, p[0] != 0);
        else if (type == SCR_REFRESH && l->screen_on) screen_all(l);
        break;
    case CH_INPUT:
        if (type == 1 && n >= 6) {
            if (pal_desktop_locked()) break; /* a locked device takes no remote keys (docs/DESKTOP.md) */
            pal_desktop_activity();          /* remote input counts as activity: the screen wakes, the idle timer restarts */
            pal_key_t k = {.sym = p[0], .mods = p[1]};
            k.cp = (uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
            pal_input_inject(&k);
        }
        break;
    case CH_FS: handle_fs(l, type, p, n); break;
    case CH_OTA: handle_ota(l, type, p, n); break;
    default: break;
    }
}

void pal_link_feed(pal_link_t *l, const uint8_t *data, size_t n) {
    if (l->kill) { /* revoked, or cable trust turned off: the session ends before anything else is read */
        link_session_end(l);
        la_sess_reset(&l->auth);
        l->trusted = false;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        switch (l->rx_state) {
        case RX_IDLE:
            if (b == MAGIC) {
                l->rx_state = RX_HEAD;
                l->idx = 0;
            } else if (l->io.passthrough) {
                size_t j = i;
                while (j < n && data[j] != MAGIC) j++;
                l->io.passthrough(l->io.ctx, data + i, j - i);
                i = j - 1;
            }
            break;
        case RX_HEAD:
            l->head[l->idx++] = b;
            if (l->idx == 4) {
                l->chan = l->head[0];
                l->type = l->head[1];
                l->len = u16(l->head + 2);
                l->idx = 0;
                if (l->len > MAX_PAYLOAD) l->rx_state = RX_IDLE;
                else l->rx_state = l->len ? RX_BODY : RX_CRC;
                l->crc_lo = 0;
            }
            break;
        case RX_BODY:
            l->rx[l->idx++] = b;
            if (l->idx == l->len) {
                l->rx_state = RX_CRC;
                l->idx = 0;
            }
            break;
        case RX_CRC:
            if (l->idx == 0) {
                l->crc_lo = b;
                l->idx = 1;
            } else {
                uint16_t want = (uint16_t)(l->crc_lo | (b << 8));
                uint16_t c = crc16(0xFFFF, l->head, 4);
                c = crc16(c, l->rx, l->len);
                l->rx_state = RX_IDLE;
                if (c == want) handle_frame(l, l->chan, l->type, l->rx, l->len);
            }
            break;
        }
    }
}

/* ---- worker ---- */

static void worker(void *arg) {
    pal_link_t *l = arg;
    uint64_t last_screen = 0;
    uint8_t buf[1 + 512];
    while (!l->dying) {
        bool have = false, exited = false;
        int n = 0;
        nema_port_mutex_lock(l->tlock);
        if (l->tty) {
            have = true;
            n = nema_tty_output_read(l->tty, buf, 512, 0); /* never sleep holding the lock: it is not fair */
            int code;
            if (l->pid && nema_proc_wait(l->pid, &code, 0) == NEMA_OK) {
                l->pid = 0;
                exited = true;
            }
        }
        nema_port_mutex_unlock(l->tlock);
        if (n > 0) send_frame(l, CH_TTY, TTY_DATA, buf, (size_t)n);
        if (exited) send_frame(l, CH_TTY, TTY_CLOSE, NULL, 0);
        /* pace outside the lock so a close or open on the receive path can always get in */
        if (n <= 0) nema_sleep_ms(10);
        else if (have) nema_sleep_ms(1);
        if (l->screen_on) {
            uint64_t now = nema_port_time_us();
            if (now - last_screen > 150000) {
                pal_rect_t r;
                bool pal;
                nema_port_mutex_lock(l->dlock);
                r = l->dirty;
                pal = l->send_palette;
                l->dirty.w = 0;
                l->send_palette = false;
                nema_port_mutex_unlock(l->dlock);
                if (pal) send_palette(l);
                if (r.w > 0) send_rect(l, r);
                last_screen = now;
            }
        }
    }
    l->worker_done = true;
}

pal_link_t *pal_link_create(const pal_link_io_t *io) {
    nema_owner_t owner = nema_owner_register("link");
    pal_link_t *l = nema_calloc(NEMA_MEM_ANY, owner, sizeof(*l));
    if (!l) return NULL;
    l->io = *io;
    l->owner = owner;
    l->tr = io->transport;
    l->trusted = link_default_trust(io->transport);
    la_init();
    l->txlock = nema_port_mutex_create();
    l->dlock = nema_port_mutex_create();
    l->tlock = nema_port_mutex_create();
    l->txbuf = nema_alloc(NEMA_MEM_ANY, owner, 7 + MAX_PAYLOAD);
    l->rx = nema_alloc(NEMA_MEM_ANY, owner, MAX_PAYLOAD);
    nema_task_desc_t td = {.name = "link", .fn = worker, .arg = l, .stack = 5120, .prio = 4, .core = -1,
                           .role = NEMA_TASK_SERVICE, .owner = owner};
    if (!l->txlock || !l->dlock || !l->tlock || !l->txbuf || !l->rx || nema_task_create(&td) != NEMA_OK) {
        l->worker_done = true;
        pal_link_destroy(l);
        return NULL;
    }
    if (link_reg_add(l) != NEMA_OK) { /* a link no revoke can reach is not allowed */
        l->worker_done = true;
        pal_link_destroy(l);
        return NULL;
    }
    return l;
}

void pal_link_destroy(pal_link_t *l) {
    if (!l) return;
    link_reg_del(l);
    l->dying = true;
    while (!l->worker_done) nema_sleep_ms(10);
    link_session_end(l);
    la_sess_reset(&l->auth);
    if (l->txlock) nema_port_mutex_destroy(l->txlock);
    if (l->dlock) nema_port_mutex_destroy(l->dlock);
    if (l->tlock) nema_port_mutex_destroy(l->tlock);
    nema_free(l->txbuf);
    nema_free(l->rx);
    nema_free(l);
}

