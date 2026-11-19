/* Runtime permissions (pal/perms.h). The answers are one small text file, /data/perms: a line per app, "id allowed denied" with the
 * two masks in hex. It is read once and rewritten whole on a change. The question is a shell dialog (any
 * session draws it; the console as text), answered on the desktop task while the app waits on a semaphore. */
#include "pal/perms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "nema/port.h"
#include "nema/vfs.h"
#include "desktop_int.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/papp.h"

#define PERMS_FILE "/data/perms"
#define PERMS_MAX 32 /* apps with answers kept */
#define ASK_MS 60000

typedef struct {
    char id[32];
    uint32_t allowed, denied;
} entry_t;

static entry_t s_e[PERMS_MAX] NEMA_EXT_BSS;
static int s_n;
static bool s_loaded; /* the file is read once; afterwards memory is the truth and the file its copy */
static nema_port_mutex_t *s_lock;
static bool (*s_asker)(const char *, uint32_t);

static void lock(void) {
    if (!s_lock) s_lock = nema_port_mutex_create();
    if (s_lock) nema_port_mutex_lock(s_lock);
}
static void unlock(void) {
    if (s_lock) nema_port_mutex_unlock(s_lock);
}

/* Lock held. */
static void load(void) {
    if (s_loaded) return;
    s_loaded = true;
    s_n = 0;
    nema_obj_t *f;
    if (nema_fs_open(PERMS_FILE, NEMA_O_READ, &f) != NEMA_OK) return;
    char *buf = nema_alloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, 2048);
    int n = buf ? nema_obj_read(f, buf, 2047, -1) : 0;
    nema_obj_unref(f);
    if (n <= 0) {
        nema_free(buf);
        return;
    }
    buf[n] = 0;
    for (char *line = strtok(buf, "\n"); line && s_n < PERMS_MAX; line = strtok(NULL, "\n")) {
        entry_t e;
        memset(&e, 0, sizeof(e));
        char hex1[12], hex2[12];
        if (sscanf(line, "%31s %11s %11s", e.id, hex1, hex2) != 3) continue;
        e.allowed = (uint32_t)strtoul(hex1, NULL, 16), e.denied = (uint32_t)strtoul(hex2, NULL, 16);
        s_e[s_n++] = e;
    }
    nema_free(buf);
}

static void save(void) {
    nema_obj_t *f;
    if (nema_fs_open(PERMS_FILE, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f) != NEMA_OK) return;
    char line[64];
    for (int i = 0; i < s_n; i++) {
        if (!s_e[i].allowed && !s_e[i].denied) continue;
        int k = snprintf(line, sizeof(line), "%s %x %x\n", s_e[i].id, (unsigned)s_e[i].allowed, (unsigned)s_e[i].denied);
        nema_obj_write(f, line, (size_t)k, 1000);
    }
    nema_obj_unref(f);
}

static entry_t *find(const char *id, bool make) {
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_e[i].id, id)) return &s_e[i];
    if (!make || s_n == PERMS_MAX) return NULL;
    entry_t *e = &s_e[s_n++];
    memset(e, 0, sizeof(*e));
    nema_strlcpy(e->id, id, sizeof(e->id));
    return e;
}

pal_perm_state_t psys_perm_state(const char *app_id, uint32_t bit) {
    if (!app_id) return PAL_PERM_ASK;
    lock();
    load();
    entry_t *e = find(app_id, false);
    pal_perm_state_t st = !e ? PAL_PERM_ASK : (e->denied & bit) ? PAL_PERM_DENIED : (e->allowed & bit) ? PAL_PERM_ALLOWED : PAL_PERM_ASK;
    unlock();
    return st;
}

nema_err_t psys_perm_set(const char *app_id, uint32_t bit, pal_perm_state_t st) {
    if (!app_id || !bit) return NEMA_ERR_INVAL;
    lock();
    load();
    entry_t *e = find(app_id, true);
    if (e) {
        e->allowed &= ~bit, e->denied &= ~bit;
        if (st == PAL_PERM_ALLOWED) e->allowed |= bit;
        else if (st == PAL_PERM_DENIED) e->denied |= bit;
        save();
    }
    unlock();
    return e ? NEMA_OK : NEMA_ERR_FULL;
}

nema_err_t psys_perm_reset(const char *app_id) {
    lock();
    load();
    entry_t *e = find(app_id, false);
    if (e) {
        e->allowed = e->denied = 0;
        save();
    }
    unlock();
    return NEMA_OK;
}

const char *psys_perm_what(uint32_t bit) {
    switch (bit) {
    case PAL_PERM_NET: return "use the network";
    case PAL_PERM_NET_LISTEN: return "accept network connections";
    case PAL_PERM_FS_HOME: return "read and change your files in /home";
    case PAL_PERM_FS_MEDIA: return "read and change the SD card";
    case PAL_PERM_PROC: return "start other programs";
    case PAL_PERM_CRYPTO_KEYS: return "use the keys in the keystore";
    case PAL_PERM_SETTINGS_WRITE: return "change system settings";
    default: return "do something sensitive";
    }
}

/* ---- the question ---- */

/* The record outlives the asker when the app is stopped before the answer: whoever comes last (the dialog's callback or the app)
 * frees it. state: 0 waiting, 1 answered, 2 the app gave up. */
typedef struct {
    nema_port_sem_t *done;
    volatile bool ok;
    int state;
} ask_t;

static void ask_free(ask_t *a) {
    nema_port_sem_destroy(a->done);
    nema_free(a);
}

static void on_answer(bool ok, void *ctx) {
    ask_t *a = (ask_t *)ctx;
    a->ok = ok;
    int was = 0;
    if (__atomic_compare_exchange_n(&a->state, &was, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) nema_port_sem_give(a->done);
    else ask_free(a); /* the app is gone: the record is ours to free */
}

/* 1 Allow, 0 Don't allow, -1 nobody can be asked (no dialog could open: no display, or no desktop yet). */
static int ask_dialog(const char *name, uint32_t bit) {
    if (!desk_started() || desk_on_task()) return -1; /* no desktop to draw it (a test, a board without one), or it would wait on itself */
    char title[48], text[96];
    snprintf(title, sizeof(title), "Allow %s?", name);
    snprintf(text, sizeof(text), "%s wants to %s.", name, psys_perm_what(bit));
    ask_t *a = nema_calloc(NEMA_MEM_ANY, NEMA_OWNER_KERNEL, sizeof(*a));
    if (!a) return 0;
    a->done = nema_port_sem_create(0, 1);
    if (!a->done || pal_dialog_confirm(title, text, "Allow", "Don't allow", on_answer, a) != NEMA_OK) {
        if (a->done) nema_port_sem_destroy(a->done);
        nema_free(a);
        return -1;
    }
    for (;;) {
        if (nema_port_sem_take(a->done, 200) == NEMA_OK) break;
        if (nema_canceled()) { /* the app is being stopped: leave the record to the callback */
            int was = 0;
            if (__atomic_compare_exchange_n(&a->state, &was, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
            break; /* the answer came at the same moment */
        }
    }
    bool ok = a->ok;
    ask_free(a);
    return ok ? 1 : 0;
}

void psys_perm_set_asker(bool (*ask)(const char *, uint32_t)) { s_asker = ask; }

bool psys_perm_check(const char *app_id, const char *app_name, uint32_t bit) {
    if (!(bit & PAL_PERM_ASKED)) return true;
    pal_perm_state_t st = psys_perm_state(app_id, bit);
    if (st != PAL_PERM_ASK) return st == PAL_PERM_ALLOWED;
    int r = s_asker ? (s_asker(app_name ? app_name : app_id, bit) ? 1 : 0) : ask_dialog(app_name ? app_name : app_id, bit);
    if (r < 0) return true; /* nobody can be asked (no display, a test): the manifest decides, as it did before, and nothing is kept */
    (void)psys_perm_set(app_id, bit, r ? PAL_PERM_ALLOWED : PAL_PERM_DENIED);
    return r == 1;
}
