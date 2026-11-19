/* Wi-Fi: the board's operations behind one set of calls, and the saved networks (pal/wifi.h). The list is kept as one blob that
 * the board stores out of reach of the shell (it holds the passwords): a magic, a count, then fixed records. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/wifi.h"

static pal_wifi_ops_t W;
static bool have;
static pal_wifi_ip_fn s_ip_fn;

static bool s_loaded;
void pal_wifi_set_ops(const pal_wifi_ops_t *ops) {
    W = *ops;
    have = true;
    s_loaded = false; /* the saved networks are read from the new store at next use */
}
bool pal_wifi_available(void) { return have; }
static size_t saved_count(void);
bool pal_wifi_has_credentials(void) {
    if (have && W.load) return saved_count() > 0;
    return have && W.has_credentials && W.has_credentials();
}
void pal_wifi_set_ip_listener(pal_wifi_ip_fn fn) { s_ip_fn = fn; }
void pal_wifi_notify_ip(bool has_ip) {
    if (s_ip_fn) s_ip_fn(has_ip);
}

nema_err_t pal_wifi_scan(pal_ap_t *out, size_t max, size_t *found) {
    return have && W.scan ? W.scan(out, max, found) : NEMA_ERR_NOSYS;
}
nema_err_t pal_wifi_connect(const char *ssid, const char *pass, int32_t timeout_ms) {
    return have && W.connect ? W.connect(ssid, pass, timeout_ms) : NEMA_ERR_NOSYS;
}
nema_err_t pal_wifi_disconnect(void) { return have && W.disconnect ? W.disconnect() : NEMA_ERR_NOSYS; }
static void list_clear(void);
nema_err_t pal_wifi_forget(void) {
    list_clear();
    return have && W.forget ? W.forget() : NEMA_ERR_NOSYS;
}

void pal_wifi_status(pal_wifi_status_t *st) {
    memset(st, 0, sizeof(*st));
    if (have && W.status) W.status(st);
}

int pal_wifi_bars(void) {
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    if (!st.connected) return 0;
    return st.rssi > -55 ? 4 : st.rssi > -65 ? 3 : st.rssi > -75 ? 2 : 1;
}

/* ---- saved networks ---- */

#define MAGIC "PWN1"

typedef struct {
    char ssid[33];
    char pass[65];
    uint8_t flags; /* 1 auto-join, 2 manual IP */
    pal_ipv4_conf_t ip;
} rec_t;

typedef struct {
    char magic[4];
    uint32_t n;
    rec_t r[PAL_WIFI_NETS];
} list_t;

static nema_port_mutex_t *s_lock;
static list_t s_list NEMA_EXT_BSS; /* the cache of the stored blob, read at first use */

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
    memset(&s_list, 0, sizeof(s_list));
    if (!have || !W.load) return;
    size_t n = W.load(&s_list, sizeof(s_list));
    if (n < 8 || memcmp(s_list.magic, MAGIC, 4) || s_list.n > PAL_WIFI_NETS || n < 8 + s_list.n * sizeof(rec_t)) {
        memset(&s_list, 0, sizeof(s_list));
        return;
    }
    for (uint32_t i = 0; i < s_list.n; i++) s_list.r[i].ssid[32] = 0, s_list.r[i].pass[64] = 0;
}

static void save(void) {
    memcpy(s_list.magic, MAGIC, 4);
    if (have && W.store) (void)W.store(&s_list, 8 + s_list.n * sizeof(rec_t));
}

static int find(const char *ssid) {
    for (uint32_t i = 0; ssid && i < s_list.n; i++)
        if (!strcmp(s_list.r[i].ssid, ssid)) return (int)i;
    return -1;
}

static void list_clear(void) {
    lock();
    load();
    memset(&s_list, 0, sizeof(s_list));
    save();
    unlock();
}

static size_t saved_count(void) {
    lock();
    load();
    size_t n = s_list.n;
    unlock();
    return n;
}

static void to_public(const rec_t *r, pal_wifi_net_t *o) {
    memset(o, 0, sizeof(*o));
    nema_strlcpy(o->ssid, r->ssid, sizeof(o->ssid));
    o->auto_join = r->flags & 1;
    o->manual_ip = r->flags & 2;
    o->secure = r->pass[0] != 0;
    o->ip = r->ip;
}

size_t pal_wifi_saved(pal_wifi_net_t *out, size_t max) {
    lock();
    load();
    size_t n = s_list.n < max ? s_list.n : max;
    for (size_t i = 0; i < n; i++) to_public(&s_list.r[i], &out[i]);
    unlock();
    return n;
}

bool pal_wifi_saved_get(const char *ssid, pal_wifi_net_t *out) {
    lock();
    load();
    int i = find(ssid);
    if (i >= 0 && out) to_public(&s_list.r[i], out);
    unlock();
    return i >= 0;
}

nema_err_t pal_wifi_forget_net(const char *ssid) {
    lock();
    load();
    int i = find(ssid);
    if (i >= 0) {
        memmove(&s_list.r[i], &s_list.r[i + 1], (s_list.n - (uint32_t)i - 1) * sizeof(rec_t));
        s_list.n--;
        memset(&s_list.r[s_list.n], 0, sizeof(rec_t));
        save();
    }
    unlock();
    if (i < 0) return NEMA_ERR_NOENT;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    if (!strcmp(st.ssid, ssid)) (void)pal_wifi_disconnect();
    return NEMA_OK;
}

nema_err_t pal_wifi_set_auto(const char *ssid, bool on) {
    lock();
    load();
    int i = find(ssid);
    if (i >= 0) {
        s_list.r[i].flags = (uint8_t)((s_list.r[i].flags & ~1u) | (on ? 1u : 0u));
        save();
    }
    unlock();
    return i >= 0 ? NEMA_OK : NEMA_ERR_NOENT;
}

nema_err_t pal_wifi_set_ip(const char *ssid, bool manual, const pal_ipv4_conf_t *conf) {
    if (manual && (!conf || !conf->ip[0])) return NEMA_ERR_INVAL;
    lock();
    load();
    int i = find(ssid);
    if (i >= 0) {
        s_list.r[i].flags = (uint8_t)((s_list.r[i].flags & ~2u) | (manual ? 2u : 0u));
        if (conf) s_list.r[i].ip = *conf;
        save();
    }
    unlock();
    return i >= 0 ? NEMA_OK : NEMA_ERR_NOENT;
}

bool pal_wifi_pick(int index, char ssid[33], char pass[65]) {
    lock();
    load();
    bool ok = false;
    for (uint32_t i = 0; i < s_list.n && !ok; i++) {
        if (!(s_list.r[i].flags & 1)) continue;
        if (index-- > 0) continue;
        nema_strlcpy(ssid, s_list.r[i].ssid, 33);
        nema_strlcpy(pass, s_list.r[i].pass, 65);
        ok = true;
    }
    unlock();
    return ok;
}

void pal_wifi_joined(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return;
    lock();
    load();
    rec_t r;
    int i = find(ssid);
    if (i >= 0) { /* known: keep its settings, take the password that worked */
        r = s_list.r[i];
        memmove(&s_list.r[i], &s_list.r[i + 1], (s_list.n - (uint32_t)i - 1) * sizeof(rec_t));
        s_list.n--;
    } else {
        memset(&r, 0, sizeof(r));
        nema_strlcpy(r.ssid, ssid, sizeof(r.ssid));
        r.flags = 1; /* a new network joins by itself */
        if (s_list.n == PAL_WIFI_NETS) s_list.n--; /* full: the least recently used goes */
    }
    nema_strlcpy(r.pass, pass ? pass : "", sizeof(r.pass));
    memmove(&s_list.r[1], &s_list.r[0], s_list.n * sizeof(rec_t));
    s_list.r[0] = r;
    s_list.n++;
    save();
    unlock();
}

bool pal_ipv4_parse(const char *text, uint8_t out[4]) {
    if (!text) return false;
    const char *p = text;
    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') return false;
        char *end;
        long v = strtol(p, &end, 10);
        if (v < 0 || v > 255 || end - p > 3) return false;
        out[i] = (uint8_t)v;
        p = end;
        if (i < 3) {
            if (*p != '.') return false;
            p++;
        }
    }
    return *p == 0;
}

void pal_ipv4_text(const uint8_t ip[4], char *out, size_t cap) { snprintf(out, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); }
