/* PalanuOS display power manager. Pure state and timers over a millisecond clock that the caller passes
 * in, so tests drive it with a fake clock. The desktop calls pal_dpm_tick on every loop and reacts to the hooks: the
 * backlight on and off, and the lock screen when the lock timeout fires. */
#include <string.h>

#include "nema/port.h"
#include "pal/dpm.h"
#include "pal/sha256.h"
#include "pal/power.h"

#define PIN_MIN 4
#define PIN_MAX 16
#define SALT_LEN 16

static struct {
    pal_dpm_hooks_t h;
    uint32_t sleep_ms, lock_ms;
    uint32_t last_act;   /* the last key or activity */
    uint32_t sleep_since; /* when the display went off (or the last refused lock attempt) */
    bool disp_on, locked;
    bool pin_set;
    uint8_t salt[SALT_LEN], hash[32];
} P = {.sleep_ms = PAL_DPM_SLEEP_DEFAULT_MS, .lock_ms = PAL_DPM_LOCK_DEFAULT_MS, .disp_on = true};

static void set_display(bool on) {
    if (P.disp_on == on) return;
    P.disp_on = on;
    if (P.h.display) P.h.display(on);
    pal_power_screen(on); /* Saver light-sleeps while the screen is off (pal/power.h) */
}

void pal_dpm_init(const pal_dpm_hooks_t *hooks, uint32_t now_ms) {
    P.h = hooks ? *hooks : (pal_dpm_hooks_t){0};
    P.last_act = now_ms;
    P.sleep_since = now_ms;
    P.disp_on = true;
    P.locked = false;
}

void pal_dpm_set_sleep_ms(uint32_t ms) { P.sleep_ms = ms; }
void pal_dpm_set_lock_ms(uint32_t ms) { P.lock_ms = ms; }
uint32_t pal_dpm_sleep_ms(void) { return P.sleep_ms; }
uint32_t pal_dpm_lock_ms(void) { return P.lock_ms; }

bool pal_dpm_key(uint32_t now_ms) {
    P.last_act = now_ms;
    if (P.disp_on) return false;
    set_display(true); /* the first key after sleep only wakes the screen */
    return true;
}

void pal_dpm_activity(uint32_t now_ms) { P.last_act = now_ms; }

void pal_dpm_tick(uint32_t now_ms) {
    if (P.disp_on) {
        if (P.sleep_ms && now_ms - P.last_act >= P.sleep_ms) {
            set_display(false);
            P.sleep_since = now_ms;
        }
        return;
    }
    if (!P.locked && P.lock_ms && now_ms - P.sleep_since >= P.lock_ms) {
        if (P.h.lock && P.h.lock()) P.locked = true;
        P.sleep_since = now_ms; /* a refused attempt is retried after another lock_ms */
    }
}

int32_t pal_dpm_next_ms(uint32_t now_ms) {
    int32_t best = -1;
    if (P.disp_on && P.sleep_ms) {
        int32_t d = (int32_t)((P.last_act + P.sleep_ms) - now_ms);
        best = d < 0 ? 0 : d;
    } else if (!P.disp_on && !P.locked && P.lock_ms) {
        int32_t d = (int32_t)((P.sleep_since + P.lock_ms) - now_ms);
        best = d < 0 ? 0 : d;
    }
    return best;
}

pal_dpm_state_t pal_dpm_state(void) {
    if (P.locked) return PAL_DPM_LOCKED;
    return P.disp_on ? PAL_DPM_ACTIVE : PAL_DPM_SLEEP;
}
bool pal_dpm_display_on(void) { return P.disp_on; }
bool pal_dpm_locked(void) { return P.locked; }

void pal_dpm_set_locked(bool locked, uint32_t now_ms) {
    P.locked = locked;
    P.last_act = now_ms;
    if (!locked && !P.disp_on) set_display(true); /* an unlock leaves the screen on */
}

/* ---- PIN: salted SHA-256 ---- */

static void pin_hash(const uint8_t salt[SALT_LEN], const char *pin, uint8_t out[32]) {
    pal_sha256_t c;
    pal_sha256_init(&c);
    pal_sha256_update(&c, salt, SALT_LEN);
    pal_sha256_update(&c, pin, strlen(pin));
    pal_sha256_final(&c, out);
}

static bool pin_ok_chars(const char *pin) {
    size_t n = strlen(pin);
    if (n < PIN_MIN || n > PIN_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)pin[i] < 0x21 || (unsigned char)pin[i] > 0x7e) return false;
    return true;
}

nema_err_t pal_dpm_pin_set(const char *pin) {
    if (!pin) return NEMA_ERR_INVAL;
    if (!pin[0]) {
        P.pin_set = false;
        memset(P.salt, 0, sizeof(P.salt));
        memset(P.hash, 0, sizeof(P.hash));
        return NEMA_OK;
    }
    if (!pin_ok_chars(pin)) return NEMA_ERR_INVAL;
    nema_port_random(P.salt, sizeof(P.salt));
    pin_hash(P.salt, pin, P.hash);
    P.pin_set = true;
    return NEMA_OK;
}

bool pal_dpm_pin_is_set(void) { return P.pin_set; }

bool pal_dpm_pin_check(const char *pin) {
    if (!P.pin_set || !pin) return false;
    uint8_t h[32];
    pin_hash(P.salt, pin, h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ P.hash[i]);
    return diff == 0;
}

static void to_hex(const uint8_t *b, size_t n, char *out) {
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = D[b[i] >> 4];
        out[2 * i + 1] = D[b[i] & 15];
    }
    out[2 * n] = 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool from_hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(s[2 * i]), lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

void pal_dpm_pin_text(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!P.pin_set || cap < 2 * (SALT_LEN + 32) + 2) return;
    char salt[2 * SALT_LEN + 1], hash[65];
    to_hex(P.salt, SALT_LEN, salt);
    to_hex(P.hash, 32, hash);
    int n = 0;
    for (const char *p = salt; *p; p++) out[n++] = *p;
    out[n++] = ':';
    for (const char *p = hash; *p; p++) out[n++] = *p;
    out[n] = 0;
}

void pal_dpm_pin_load(const char *text) {
    P.pin_set = false;
    if (!text || strlen(text) != 2 * SALT_LEN + 1 + 64 || text[2 * SALT_LEN] != ':') return;
    if (!from_hex(text, P.salt, SALT_LEN) || !from_hex(text + 2 * SALT_LEN + 1, P.hash, 32)) return;
    P.pin_set = true;
}
