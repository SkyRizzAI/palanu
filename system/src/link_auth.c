/* Palanu Link pairing and authentication: the state machine of docs/PLP.md. Keys and their derivation:
 *   shared  = X25519(device ephemeral secret, client ephemeral public)
 *   prk     = HMAC-SHA256(key = "palanu-pair-v1", shared || code)           (the code joins the shared secret)
 *   k_mac   = HMAC-SHA256(prk, "mac" || transcript)     transcript = cpub || dpub || cn || dn (96 bytes)
 *   k_enc   = HMAC-SHA256(prk, "enc" || transcript)
 *   proof   = HMAC-SHA256(k_mac, "pair-proof" || transcript)                 (PAIR_PROOF)
 *   token   = ChaCha20-Poly1305(k_enc, nonce, aad = "palanu-token" || id)    (PAIR_OK)
 *   auth    = HMAC-SHA256(token, nonce || client id || transport tag)        (AUTH_RESPONSE)
 * The window, the rate limit and the client table are device-wide and share one lock. A connection's exchange state is
 * owned by its link task. */
#include <stdio.h>
#include <string.h>

#include "../../third_party/monocypher/monocypher.h"
#include "crypto_mac.h"
#include "crypto_prim.h"
#include "cryptoimpl.h"
#include "link_auth.h"
#include "link_clients.h"
#include "nema/log.h"
#include "nema/port.h"
#include "nema/types.h"
#include "pal/time.h"

#define T_CPUB 0
#define T_DPUB 32
#define T_CN 64
#define T_DN 80

static la_env_t s_env;
static nema_port_mutex_t *s_mu;

static uint32_t sys_now_ms(void) { return (uint32_t)(nema_port_time_us() / 1000u); }
static void sys_random(void *buf, size_t n) { nema_port_random(buf, n); }

void la_set_env(const la_env_t *env) {
    s_env.now_ms = env && env->now_ms ? env->now_ms : sys_now_ms;
    s_env.random = env && env->random ? env->random : sys_random;
}

static uint32_t now(void) {
    if (!s_env.now_ms) la_set_env(NULL);
    return s_env.now_ms();
}

static void rnd(void *buf, size_t n) {
    if (!s_env.random) la_set_env(NULL);
    s_env.random(buf, n);
}

void la_init(void) {
    if (!s_env.now_ms) la_set_env(NULL);
    if (!s_mu) s_mu = nema_port_mutex_create();
}

static void lock(void) {
    if (!s_mu) la_init();
    nema_port_mutex_lock(s_mu);
}
static void unlock(void) { nema_port_mutex_unlock(s_mu); }

/* ---- helpers ---- */

typedef struct {
    const void *p;
    size_t n;
} part_t;

/* HMAC-SHA256 over the concatenation of the parts. */
static void mac_parts(uint8_t out[32], const uint8_t *key, size_t klen, const part_t *parts, size_t np) {
    crypto_mac_t m;
    crypto_mac_hmac(&m, CI_HASH_SHA256, key, klen);
    for (size_t i = 0; i < np; i++) crypto_mac_update(&m, parts[i].p, parts[i].n);
    crypto_mac_final(&m, out);
}

static void reply(la_reply_t *r, uint8_t type, const void *p, size_t n) {
    r->send = true;
    r->type = type;
    r->len = n;
    if (n) memcpy(r->payload, p, n);
}

static void reply_err(la_reply_t *r, int code, const char *msg) {
    uint8_t b[64];
    size_t n = strlen(msg);
    if (n > 60) n = 60;
    b[0] = (uint8_t)(int8_t)code;
    memcpy(b + 1, msg, n);
    reply(r, LA_TYPE_ERR, b, n + 1);
}

/* ---- the pairing window ---- */

static struct {
    bool open;
    uint32_t deadline;
    int tries;
    char code[PAL_LINK_CODE_LEN + 1];
} s_win;

/* True while the window is open with tries left. Closes it when its time is up. Call with the lock held. */
static bool window_live(void) {
    if (s_win.open && now() >= s_win.deadline) {
        s_win.open = false;
        memset(s_win.code, 0, sizeof(s_win.code));
    }
    return s_win.open && s_win.tries > 0;
}

static void window_close(void) {
    s_win.open = false;
    s_win.tries = 0;
    memset(s_win.code, 0, sizeof(s_win.code));
}

nema_err_t la_pair_open(char code[PAL_LINK_CODE_LEN + 1]) {
    uint32_t v;
    do {
        rnd(&v, sizeof(v));
    } while (v >= 4294000000u); /* a multiple of 10^6 below 2^32, so the six digits stay uniform */
    unsigned long six = v % 1000000u; /* a fresh local: GCC sees its range (v escaped to rnd), so the six digits fit */
    lock();
    snprintf(s_win.code, sizeof(s_win.code), "%06lu", six);
    s_win.open = true;
    s_win.deadline = now() + LA_WINDOW_MS;
    s_win.tries = LA_TRIES;
    memcpy(code, s_win.code, PAL_LINK_CODE_LEN + 1);
    unlock();
    NEMA_LOGI("link", "pairing code %s (valid 120 s, %d tries)", code, LA_TRIES);
    return NEMA_OK;
}

void la_pair_close(void) {
    lock();
    window_close();
    unlock();
}

size_t la_pair_status(char *out, size_t cap) {
    lock();
    bool live = window_live();
    int n = live ? snprintf(out, cap, "pairing window open: %lu s left, %d tries left",
                            (unsigned long)((s_win.deadline - now()) / 1000u), s_win.tries)
                 : snprintf(out, cap, "pairing window closed");
    unlock();
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ---- rate limit per transport (index by transport, 1..4) ---- */

static struct {
    uint8_t fails;
    uint32_t until; /* 0 when not backing off */
} s_rl[5];

static int rl_index(pal_link_transport_t tr) { return (tr >= PAL_LINK_UART && tr <= PAL_LINK_WS) ? (int)tr : 0; }

static bool backoff_locked(pal_link_transport_t tr) {
    int i = rl_index(tr);
    if (!s_rl[i].until) return false;
    if (now() < s_rl[i].until) return true;
    s_rl[i].until = 0;
    return false;
}

bool la_backoff(pal_link_transport_t tr) {
    lock();
    bool b = backoff_locked(tr);
    unlock();
    return b;
}

void la_backoff_reset(void) {
    lock();
    memset(s_rl, 0, sizeof(s_rl));
    unlock();
}

/* Counts a failed AUTH_RESPONSE; the fifth in a row starts the back-off. */
static void rl_failed(pal_link_transport_t tr) {
    lock();
    int i = rl_index(tr);
    if (++s_rl[i].fails >= LA_FAILS_MAX) {
        s_rl[i].fails = 0;
        s_rl[i].until = now() + LA_BACKOFF_MS;
        NEMA_LOGW("link", "too many failed logins on transport %d: waiting 30 s", (int)tr);
    }
    unlock();
}

static void rl_ok(pal_link_transport_t tr) {
    lock();
    s_rl[rl_index(tr)].fails = 0;
    unlock();
}

/* ---- session ---- */

/* Forgets the pairing exchange (and its secrets). */
static void pair_reset(la_sess_t *s) {
    memset(s->shared, 0, sizeof(s->shared));
    memset(s->transcript, 0, sizeof(s->transcript));
    s->pair_state = 0;
}

void la_sess_reset(la_sess_t *s) {
    pair_reset(s);
    s->auth_open = 0;
    s->authed = 0;
    memset(s->client_id, 0, sizeof(s->client_id));
}

/* Derives k_mac (keys[0..32)) and k_enc (keys[32..64)) from the shared secret, the code and the transcript. */
static void pair_keys(uint8_t keys[64], const la_sess_t *s, const char code[PAL_LINK_CODE_LEN]) {
    uint8_t prk[32];
    part_t a[2] = {{s->shared, 32}, {code, PAL_LINK_CODE_LEN}};
    mac_parts(prk, (const uint8_t *)"palanu-pair-v1", 14, a, 2);
    part_t m[2] = {{"mac", 3}, {s->transcript, 96}};
    part_t e[2] = {{"enc", 3}, {s->transcript, 96}};
    mac_parts(keys, prk, 32, m, 2);
    mac_parts(keys + 32, prk, 32, e, 2);
    crypto_wipe(prk, sizeof(prk));
}

static void on_pair_request(la_sess_t *s, la_reply_t *r, const uint8_t *p, size_t n) {
    pair_reset(s);
    if (n < 1 || p[0] < 1 || p[0] > LC_NAME_MAX || n != 1u + p[0] + 48u)
        return reply_err(r, NEMA_ERR_INVAL, "bad PAIR_REQUEST");
    lock();
    bool live = window_live();
    int count = live ? lc_count() : 0;
    unlock();
    if (!live) return reply_err(r, PAL_LINK_E_PAIR_CLOSED, "no pairing window: run `link pair` on the device");
    if (count >= LC_MAX) return reply_err(r, NEMA_ERR_FULL, "too many paired clients: revoke one first");

    size_t nl = p[0];
    for (size_t i = 0; i < nl; i++) { /* the name is printed by `link list`: no control characters reach the terminal */
        uint8_t c = p[1 + i];
        s->name[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    s->name[nl] = 0;
    const uint8_t *cpub = p + 1 + nl, *cn = cpub + 32;
    uint8_t dsk[32], dpub[32], dn[16];
    rnd(dsk, sizeof(dsk));
    rnd(dn, sizeof(dn));
    prim_x25519_base(dpub, dsk);
    int rc = prim_x25519(s->shared, dsk, cpub);
    crypto_wipe(dsk, sizeof(dsk));
    if (rc != NEMA_OK) {
        pair_reset(s);
        return reply_err(r, NEMA_ERR_AUTH, "bad client key");
    }
    memcpy(s->transcript + T_CPUB, cpub, 32);
    memcpy(s->transcript + T_DPUB, dpub, 32);
    memcpy(s->transcript + T_CN, cn, 16);
    memcpy(s->transcript + T_DN, dn, 16);
    s->pair_state = 1;
    uint8_t out[48];
    memcpy(out, dpub, 32);
    memcpy(out + 32, dn, 16);
    reply(r, LA_TYPE_PAIR_CHALLENGE, out, sizeof(out));
}

/* A wrong code costs a try for the whole window, not for the connection: reconnecting does not reset it. */
static void pair_wrong(la_sess_t *s, la_reply_t *r) {
    if (s_win.tries > 0) s_win.tries--;
    int left = s_win.tries;
    if (left == 0) window_close();
    unlock();
    if (left == 0) {
        pair_reset(s);
        return reply_err(r, PAL_LINK_E_PAIR_CLOSED, "wrong code; the pairing window is closed");
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "wrong code, %d %s left", left, left == 1 ? "try" : "tries");
    reply_err(r, NEMA_ERR_AUTH, msg);
}

/* Creates the client record with a fresh id (checked against the store) and a fresh token. Lock held. */
static int new_client(lc_rec_t *rec, const char *name) {
    memset(rec, 0, sizeof(*rec));
    for (int i = 0; i < 8; i++) {
        lc_rec_t probe;
        rnd(rec->id, 8);
        if (lc_load(rec->id, &probe) == NEMA_ERR_NOENT) break;
        if (i == 7) return NEMA_ERR_EXIST;
    }
    rnd(rec->token, sizeof(rec->token));
    nema_strlcpy(rec->name, name, sizeof(rec->name));
    rec->created = pal_time_epoch();
    rec->last_used = 0;
    return NEMA_OK;
}

static void on_pair_proof(la_sess_t *s, la_reply_t *r, const uint8_t *p, size_t n) {
    lock();
    bool live = window_live(); /* the window is checked first: after it closes, the answer is always "closed" */
    unlock();
    if (!live) {
        pair_reset(s);
        return reply_err(r, PAL_LINK_E_PAIR_CLOSED, "pairing window closed");
    }
    if (s->pair_state != 1) return reply_err(r, NEMA_ERR_STATE, "send PAIR_REQUEST first");
    if (n != 32) return reply_err(r, NEMA_ERR_INVAL, "bad PAIR_PROOF");
    lock();
    if (!window_live()) {
        unlock();
        pair_reset(s);
        return reply_err(r, PAL_LINK_E_PAIR_CLOSED, "pairing window closed");
    }
    uint8_t keys[64], want[32];
    pair_keys(keys, s, s_win.code);
    part_t pr[2] = {{"pair-proof", 10}, {s->transcript, 96}};
    mac_parts(want, keys, 32, pr, 2);
    if (crypto_verify32(want, p) != 0) {
        crypto_wipe(keys, sizeof(keys));
        return pair_wrong(s, r);
    }
    /* the code is right: create the client, close the window (single use), and send the token under k_enc */
    lc_rec_t rec;
    int rc = new_client(&rec, s->name);
    if (rc == NEMA_OK) rc = lc_store(&rec);
    window_close();
    unlock();
    pair_reset(s);
    if (rc != NEMA_OK) {
        crypto_wipe(keys, sizeof(keys));
        crypto_wipe(rec.token, sizeof(rec.token));
        return reply_err(r, rc, "cannot store the new client");
    }
    NEMA_LOGI("link", "paired client %s", rec.name);
    uint8_t out[8 + 12 + 32 + 16], nonce[12], aad[12 + 8];
    memcpy(out, rec.id, 8);
    rnd(nonce, sizeof(nonce));
    memcpy(out + 8, nonce, 12);
    memcpy(out + 20, rec.token, 32);
    memcpy(aad, "palanu-token", 12);
    memcpy(aad + 12, rec.id, 8);
    prim_aead_seal(keys + 32, nonce, aad, sizeof(aad), out + 20, 32, out + 52);
    crypto_wipe(keys, sizeof(keys));
    crypto_wipe(rec.token, sizeof(rec.token));
    reply(r, LA_TYPE_PAIR_OK, out, sizeof(out));
}

static void on_auth_hello(la_sess_t *s, pal_link_transport_t tr, la_reply_t *r, const uint8_t *p, size_t n) {
    if (n != 8) return reply_err(r, NEMA_ERR_INVAL, "bad AUTH_HELLO");
    if (la_backoff(tr)) return reply_err(r, NEMA_ERR_BUSY, "too many failed logins; wait 30 s");
    memcpy(s->auth_id, p, 8);
    rnd(s->auth_nonce, sizeof(s->auth_nonce));
    s->auth_open = 1;
    reply(r, LA_TYPE_AUTH_CHALLENGE, s->auth_nonce, sizeof(s->auth_nonce));
}

static void on_auth_response(la_sess_t *s, pal_link_transport_t tr, la_reply_t *r, const uint8_t *p, size_t n) {
    if (!s->auth_open) return reply_err(r, NEMA_ERR_STATE, "send AUTH_HELLO first");
    s->auth_open = 0; /* one response per challenge */
    if (n != 32) return reply_err(r, NEMA_ERR_INVAL, "bad AUTH_RESPONSE");
    if (la_backoff(tr)) return reply_err(r, NEMA_ERR_BUSY, "too many failed logins; wait 30 s");
    lc_rec_t rec;
    int found = lc_load(s->auth_id, &rec);
    uint8_t token[32];
    if (found == NEMA_OK) memcpy(token, rec.token, 32);
    else rnd(token, sizeof(token)); /* an unknown client costs the same as a known one, and is refused below */
    uint8_t tag = (uint8_t)tr;
    part_t parts[3] = {{s->auth_nonce, 16}, {s->auth_id, 8}, {&tag, 1}};
    uint8_t want[32];
    mac_parts(want, token, 32, parts, 3);
    crypto_wipe(token, sizeof(token));
    if (found != NEMA_OK || crypto_verify32(want, p) != 0) {
        rl_failed(tr);
        return reply_err(r, NEMA_ERR_AUTH, "authentication failed");
    }
    rl_ok(tr);
    rec.last_used = pal_time_epoch();
    lc_store(&rec); /* a failed bookkeeping write does not refuse a good login */
    s->authed = 1;
    memcpy(s->client_id, s->auth_id, 8);
    r->trust = true;
    reply(r, LA_TYPE_AUTH_OK, NULL, 0);
}

void la_handle(la_sess_t *s, pal_link_transport_t tr, uint8_t type, const uint8_t *p, size_t n, la_reply_t *r) {
    r->send = false;
    r->trust = false;
    switch (type) {
    case LA_TYPE_PAIR_REQUEST: on_pair_request(s, r, p, n); break;
    case LA_TYPE_PAIR_PROOF: on_pair_proof(s, r, p, n); break;
    case LA_TYPE_AUTH_HELLO: on_auth_hello(s, tr, r, p, n); break;
    case LA_TYPE_AUTH_RESPONSE: on_auth_response(s, tr, r, p, n); break;
    default: break;
    }
}
