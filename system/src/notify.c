/* Notifications and toasts: the ring, the toast queue, the rate limit, and the sink. Shared by every app and the
 * system, so one mutex guards it; the sink runs after the lock is released. */
#include <string.h>

#include "nema/log.h"
#include "nema/port.h"
#include "nema/types.h"
#include "pal/board.h"
#include "pal/notify.h"
#include "pal/settings.h"
#include "pal/time.h"

#define RATE_MAX 4              /* posts (notifications and toasts) per app per window */
#define RATE_WINDOW_US 1000000u /* one second */
#define RATE_SLOTS 8            /* apps tracked at once; the least recently started window is reused */
#define TOAST_MS_MIN 100
#define TOAST_MS_MAX 10000
#define FLAGS_KNOWN PSYS_NOTIFY_ALERT

typedef struct {
    char source[PSYS_SOURCE_MAX];
    uint64_t start_us;
    uint32_t n;
    int used;
} rate_t;

static struct {
    nema_port_mutex_t *lock;
    psys_notify_sink_t sink;
    psys_notify_t ring[PSYS_NOTIFY_RING]; /* ring[0] is the oldest */
    int count;
    uint32_t last_id; /* the id given last; the first is 1 */
    psys_toast_t toasts[PSYS_TOAST_QUEUE];
    int toast_head, toast_count;
    rate_t rate[RATE_SLOTS];
} N NEMA_EXT_BSS; /* zero at boot: in external RAM where the board has it (nema/types.h) */

static void lock(void) {
    if (!__atomic_load_n(&N.lock, __ATOMIC_ACQUIRE)) {
        nema_port_mutex_t *m = nema_port_mutex_create();
        if (!m) nema_panic("notify: no mutex");
        nema_port_mutex_t *expect = NULL;
        if (!__atomic_compare_exchange_n(&N.lock, &expect, m, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            nema_port_mutex_destroy(m);
    }
    nema_port_mutex_lock(N.lock);
}

static void unlock(void) { nema_port_mutex_unlock(N.lock); }

static void sink_call(psys_notify_change_t change, const psys_notify_t *n) {
    psys_notify_sink_t s;
    lock();
    s = N.sink;
    unlock();
    if (s) s(change, n);
}

/* Counts one post of source in the window that holds now. False when the app is over its rate. Locked by the caller. */
static int rate_take(const char *source, uint64_t now) {
    rate_t *r = NULL;
    for (int i = 0; i < RATE_SLOTS && !r; i++)
        if (N.rate[i].used && strcmp(N.rate[i].source, source) == 0) r = &N.rate[i];
    if (!r) {
        r = &N.rate[0];
        for (int i = 0; i < RATE_SLOTS; i++) {
            if (!N.rate[i].used) {
                r = &N.rate[i];
                break;
            }
            if (N.rate[i].start_us < r->start_us) r = &N.rate[i];
        }
        nema_strlcpy(r->source, source, sizeof(r->source));
        r->used = 1;
        r->start_us = now;
        r->n = 0;
    }
    if (now - r->start_us >= RATE_WINDOW_US) {
        r->start_us = now;
        r->n = 0;
    }
    if (r->n >= RATE_MAX) return 0;
    r->n++;
    return 1;
}

static int text_ok(const char *s, size_t cap) { return s && strlen(s) < cap; }

static psys_notify_t *find_locked(int32_t id, int *index) {
    for (int i = 0; i < N.count; i++)
        if (N.ring[i].id == (uint32_t)id) {
            if (index) *index = i;
            return &N.ring[i];
        }
    return NULL;
}

int32_t psys_notify_post(const char *source, const char *title, const char *text, int32_t flags) {
    if (!source || !text_ok(title, PSYS_NOTIFY_TITLE) || !text_ok(text, PSYS_NOTIFY_TEXT)) return NEMA_ERR_INVAL;
    if (flags & ~FLAGS_KNOWN) return NEMA_ERR_INVAL;
    psys_notify_t snap;
    lock();
    if (!rate_take(source, nema_port_time_us())) {
        unlock();
        return NEMA_ERR_BUSY;
    }
    if (N.count == PSYS_NOTIFY_RING) {
        memmove(&N.ring[0], &N.ring[1], (PSYS_NOTIFY_RING - 1) * sizeof(N.ring[0]));
        N.count--;
    }
    psys_notify_t *n = &N.ring[N.count++];
    memset(n, 0, sizeof(*n));
    n->id = ++N.last_id;
    nema_strlcpy(n->source, source, sizeof(n->source));
    nema_strlcpy(n->title, title, sizeof(n->title));
    nema_strlcpy(n->text, text, sizeof(n->text));
    n->time = pal_time_epoch();
    n->flags = flags;
    snap = *n;
    unlock();
    NEMA_LOGI("notify", "%s: %s: %s", source, title, text);
    /* Settings: notify.dnd silences the beep (and the toasts, below); notify.beep turns the buzzer on for a post. */
    if (!psys_settings_int("notify.dnd", 0) && psys_settings_int("notify.beep", 1)) pal_beep(60);
    sink_call(PSYS_NOTIFY_POSTED, &snap);
    return (int32_t)snap.id;
}

int32_t psys_notify_update(const char *source, int32_t id, const char *title, const char *text) {
    if (!source || !text_ok(title, PSYS_NOTIFY_TITLE) || !text_ok(text, PSYS_NOTIFY_TEXT)) return NEMA_ERR_INVAL;
    psys_notify_t snap;
    lock();
    psys_notify_t *n = find_locked(id, NULL);
    if (!n) {
        unlock();
        return NEMA_ERR_NOENT;
    }
    if (strcmp(n->source, source) != 0) {
        unlock();
        return NEMA_ERR_PERM;
    }
    memset(n->title, 0, sizeof(n->title));
    memset(n->text, 0, sizeof(n->text));
    nema_strlcpy(n->title, title, sizeof(n->title));
    nema_strlcpy(n->text, text, sizeof(n->text));
    n->read = false;
    snap = *n;
    unlock();
    sink_call(PSYS_NOTIFY_UPDATED, &snap);
    return NEMA_OK;
}

int32_t psys_notify_cancel(const char *source, int32_t id) {
    if (!source) return NEMA_ERR_INVAL;
    psys_notify_t snap;
    lock();
    int i = -1;
    psys_notify_t *n = find_locked(id, &i);
    if (!n) {
        unlock();
        return NEMA_ERR_NOENT;
    }
    if (strcmp(n->source, source) != 0) {
        unlock();
        return NEMA_ERR_PERM;
    }
    snap = *n;
    memmove(&N.ring[i], &N.ring[i + 1], (size_t)(N.count - i - 1) * sizeof(N.ring[0]));
    N.count--;
    unlock();
    sink_call(PSYS_NOTIFY_CANCELED, &snap);
    return NEMA_OK;
}

int32_t psys_toast_post(const char *source, const char *text, int32_t ms) {
    if (!source || !text_ok(text, PSYS_NOTIFY_TEXT) || ms < TOAST_MS_MIN || ms > TOAST_MS_MAX) return NEMA_ERR_INVAL;
    if (psys_settings_int("notify.dnd", 0)) return 0; /* Do not disturb: no toast */
    lock();
    if (!rate_take(source, nema_port_time_us())) {
        unlock();
        return NEMA_ERR_BUSY;
    }
    if (N.toast_count == PSYS_TOAST_QUEUE) {
        unlock();
        return NEMA_ERR_FULL;
    }
    psys_toast_t *t = &N.toasts[(N.toast_head + N.toast_count) % PSYS_TOAST_QUEUE];
    memset(t, 0, sizeof(*t));
    nema_strlcpy(t->source, source, sizeof(t->source));
    nema_strlcpy(t->text, text, sizeof(t->text));
    t->ms = ms;
    N.toast_count++;
    unlock();
    NEMA_LOGI("notify", "toast %s: %s", source, text);
    sink_call(PSYS_NOTIFY_TOASTED, NULL);
    return NEMA_OK;
}

void psys_notify_set_sink(psys_notify_sink_t sink) {
    lock();
    N.sink = sink;
    unlock();
}

int psys_notify_count(void) {
    lock();
    int n = N.count;
    unlock();
    return n;
}

int psys_notify_unread_count(void) {
    lock();
    int k = 0;
    for (int i = 0; i < N.count; i++)
        if (!N.ring[i].read) k++;
    unlock();
    return k;
}

int psys_notify_list(psys_notify_t *out, int cap) {
    lock();
    int k = 0;
    for (int i = N.count - 1; i >= 0 && k < cap; i--) out[k++] = N.ring[i];
    unlock();
    return k;
}

void psys_notify_mark_all_read(void) {
    lock();
    for (int i = 0; i < N.count; i++) N.ring[i].read = true;
    unlock();
}

void psys_notify_clear(void) {
    lock();
    N.count = 0;
    unlock();
    sink_call(PSYS_NOTIFY_CANCELED, NULL);
}

int psys_toast_take(psys_toast_t *out) {
    lock();
    if (N.toast_count == 0) {
        unlock();
        return 0;
    }
    *out = N.toasts[N.toast_head];
    N.toast_head = (N.toast_head + 1) % PSYS_TOAST_QUEUE;
    N.toast_count--;
    unlock();
    return 1;
}
