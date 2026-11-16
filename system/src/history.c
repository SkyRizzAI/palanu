/* Shell history kept across restarts (system/include/pal/history.h). A ring of lines in external RAM, loaded from the
 * file at first use, written back by a shell that sits idle at its prompt. */
#include "pal/history.h"

#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "nema/port.h"

static struct {
    nema_port_mutex_t *lock;
    bool loaded;
    bool writing;          /* a shell is writing the file; the others skip */
    uint32_t gen, saved;   /* changes made, and the change count the file holds */
    uint64_t changed_us;   /* the last change, or the earliest next try after a failed write */
    int head, n;           /* ring: line i (0 oldest) is at (head + i) % PAL_HISTORY_MAX */
    char path[64];
    uint64_t (*now_us)(void);
    char line[PAL_HISTORY_MAX][PAL_HISTORY_LINE];
} H NEMA_EXT_BSS; /* 20 KB, zero at boot: in external RAM where the board has it */

static void lock(void) {
    if (!__atomic_load_n(&H.lock, __ATOMIC_ACQUIRE)) {
        nema_port_mutex_t *m = nema_port_mutex_create();
        if (!m) nema_panic("history: no mutex");
        nema_port_mutex_t *expect = NULL;
        if (!__atomic_compare_exchange_n(&H.lock, &expect, m, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            nema_port_mutex_destroy(m);
    }
    nema_port_mutex_lock(H.lock);
}

static void unlock(void) { nema_port_mutex_unlock(H.lock); }

static uint64_t now_us(void) { return H.now_us ? H.now_us() : nema_port_time_us(); }
static const char *path(void) { return H.path[0] ? H.path : PAL_HISTORY_FILE; }
static char *at(int i) { return H.line[(H.head + i) % PAL_HISTORY_MAX]; }

static bool keep(const char *s) {
    if (!s || !*s || *s == ' ') return false;
    for (const char *p = s; *p; p++)
        if (*p != ' ' && *p != '\t') return true;
    return false;
}

/* Lock held. Appends one line; the oldest goes when the ring is full. */
static void push(const char *s, size_t len) {
    if (len >= PAL_HISTORY_LINE) len = PAL_HISTORY_LINE - 1;
    char *slot;
    if (H.n < PAL_HISTORY_MAX) slot = at(H.n++);
    else {
        slot = H.line[H.head];
        H.head = (H.head + 1) % PAL_HISTORY_MAX;
    }
    memcpy(slot, s, len);
    slot[len] = 0;
}

/* Lock held. Reads the file once; a missing or unreadable file is an empty history. */
static void load(void) {
    if (H.loaded) return;
    H.loaded = true;
    nema_obj_t *f;
    if (nema_fs_open(path(), NEMA_O_READ, &f) != NEMA_OK) return;
    char buf[512], cur[PAL_HISTORY_LINE];
    size_t len = 0;
    bool skip = false; /* the rest of a line longer than PAL_HISTORY_LINE */
    int r;
    while ((r = nema_obj_read(f, buf, sizeof(buf), -1)) > 0) {
        for (int i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n') {
                cur[len] = 0;
                if (!skip && keep(cur)) push(cur, len);
                len = 0;
                skip = false;
            } else if (c != '\r' && !skip) {
                if (len < sizeof(cur) - 1) cur[len++] = c;
                else skip = true;
            }
        }
    }
    cur[len] = 0;
    if (!skip && keep(cur)) push(cur, len);
    nema_obj_unref(f);
}

void pal_history_add(const char *s) {
    if (!keep(s)) return;
    size_t len = strlen(s);
    lock();
    load();
    if (H.n == 0 || strncmp(at(H.n - 1), s, PAL_HISTORY_LINE - 1) != 0) {
        push(s, len);
        H.gen++;
        H.changed_us = now_us();
    }
    unlock();
}

int pal_history_count(void) {
    lock();
    load();
    int n = H.n;
    unlock();
    return n;
}

bool pal_history_get(int i, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    lock();
    load();
    bool ok = i >= 0 && i < H.n;
    if (ok) nema_strlcpy(out, at(i), cap);
    unlock();
    return ok;
}

int32_t pal_history_flush_in_ms(void) {
    lock();
    int32_t ms = -1;
    if (H.gen != H.saved && !H.writing) {
        uint64_t due = H.changed_us + (uint64_t)PAL_HISTORY_IDLE_MS * 1000u, now = now_us();
        ms = now >= due ? 0 : (int32_t)((due - now) / 1000u) + 1;
    }
    unlock();
    return ms;
}

static nema_err_t ensure_dir(const char *file) {
    char dir[64];
    nema_strlcpy(dir, file, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return NEMA_OK;
    *slash = 0;
    nema_stat_t st;
    if (nema_fs_stat(dir, &st) == NEMA_OK) return NEMA_OK;
    nema_err_t e = nema_fs_mkdir(dir);
    return e == NEMA_OK || e == NEMA_ERR_EXIST ? NEMA_OK : e;
}

nema_err_t pal_history_flush(void) {
    lock();
    load();
    if (H.gen == H.saved || H.writing) {
        unlock();
        return NEMA_OK;
    }
    /* Copy the text out, so the write (tens of ms on flash) runs without the lock and other shells keep adding. */
    size_t cap = 1, used = 0;
    for (int i = 0; i < H.n; i++) cap += strlen(at(i)) + 1;
    char *text = (char *)nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, cap);
    if (!text) {
        unlock();
        return NEMA_ERR_NOMEM;
    }
    for (int i = 0; i < H.n; i++) {
        size_t l = strlen(at(i));
        memcpy(text + used, at(i), l);
        used += l;
        text[used++] = '\n';
    }
    uint32_t gen = H.gen;
    H.writing = true;
    char file[64], tmp[72];
    nema_strlcpy(file, path(), sizeof(file));
    unlock();

    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    nema_err_t e = ensure_dir(file);
    nema_obj_t *f = NULL;
    if (e == NEMA_OK) e = nema_fs_open(tmp, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f);
    if (e == NEMA_OK) {
        int w = used ? nema_obj_write(f, text, used, -1) : 0;
        nema_obj_unref(f);
        e = w == (int)used ? NEMA_OK : NEMA_ERR_IO;
    }
    if (e == NEMA_OK) e = nema_fs_rename(tmp, file);
    if (e != NEMA_OK) nema_fs_unlink(tmp);
    nema_free(text);

    lock();
    H.writing = false;
    if (e == NEMA_OK) H.saved = gen;
    else H.changed_us = now_us() + (uint64_t)(PAL_HISTORY_RETRY_MS - PAL_HISTORY_IDLE_MS) * 1000u; /* try again later */
    unlock();
    if (e != NEMA_OK) NEMA_LOGW("history", "cannot write %s: %d", file, (int)e);
    return e;
}

void pal_history_reset_for_test(const char *p, uint64_t (*clock)(void)) {
    lock();
    H.loaded = false;
    H.writing = false;
    H.gen = H.saved = 0;
    H.changed_us = 0;
    H.head = H.n = 0;
    nema_strlcpy(H.path, p ? p : "", sizeof(H.path));
    H.now_us = clock;
    unlock();
}
