#include <stdio.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/desktop.h"
#include "pal/input.h"
#include "pal/perf.h"

#define QUEUE_LEN 32
#define REPEAT_DELAY_MS 420 /* the defaults; pal_input_set_repeat changes them (Settings: input.repeat_delay/rate) */
#define REPEAT_RATE_MS 55
#define LONG_PRESS_MS 550

static uint32_t s_repeat_delay = REPEAT_DELAY_MS, s_repeat_rate = REPEAT_RATE_MS;

void pal_input_set_repeat(uint32_t delay_ms, uint32_t rate_ms) {
    if (delay_ms >= 120 && delay_ms <= 1000) s_repeat_delay = delay_ms;
    if (rate_ms >= 20 && rate_ms <= 200) s_repeat_rate = rate_ms;
}
void pal_input_get_repeat(uint32_t *delay_ms, uint32_t *rate_ms) {
    if (delay_ms) *delay_ms = s_repeat_delay;
    if (rate_ms) *rate_ms = s_repeat_rate;
}
uint32_t pal_input_long_press_ms(void) { return LONG_PRESS_MS; }
#define FN_TAP_MAX_MS 300  /* a tap of Fn is shorter than this */
#define FN_GAP_MAX_MS 350  /* the second tap starts this soon after the first one ends */

static struct {
    const pal_keyboard_t *kb;
    nema_port_mutex_t *lock;
    pal_key_t q[QUEUE_LEN];
    int head, count;
    nema_obj_t obj;

    int shift, ctrl, alt, fn, nm; /* number of physical keys of each kind currently held */
    bool nm_latch;                /* Nm tapped: applies to the next character only */
    bool nm_used;                 /* another key was pressed while Nm was held */
    bool mode[PAL_INPUT_MODES];   /* the sticky modes (pal_input_mode_t) */
    /* Shift tapped twice: Caps Lock. clean = no other key went down while Shift was held */
    bool shift_clean, shift_tap_pending;
    uint32_t shift_down_ms, shift_tap_up_ms;

    /* repeat */
    bool rep_active;
    const pal_physkey_t *rep_pk; /* the physical key that is repeating: releasing another key must not stop it */
    pal_key_t rep_key;
    uint32_t rep_next_ms;
    /* cycle key */
    bool cycle_down, cycle_fired;
    uint32_t cycle_at_ms;
    /* Fn double tap (PAL_KEY_VKB): clean = no other key went down while Fn was held */
    bool fn_clean, fn_tap_pending, fn_tap_report;
    uint32_t fn_down_ms, fn_tap_up_ms;
    uint32_t last_ms;
    uint32_t (*clock)(void);
} I;

static int in_read(nema_obj_t *o, void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static int in_write(nema_obj_t *o, const void *b, size_t n) { (void)o, (void)b, (void)n; return NEMA_ERR_PERM; }
static uint32_t in_poll(nema_obj_t *o, uint32_t ev) {
    (void)o, (void)ev;
    return I.count > 0 ? NEMA_POLL_IN : 0;
}
static nema_err_t in_ioctl(nema_obj_t *o, uint32_t r, void *a) { (void)o, (void)r, (void)a; return NEMA_ERR_NOSYS; }
static const nema_obj_ops_t IN_OPS = {"input", in_read, in_write, in_poll, in_ioctl, NULL};

void pal_input_init(const pal_keyboard_t *kb) {
    memset(&I, 0, sizeof(I));
    I.kb = kb;
    I.lock = nema_port_mutex_create();
    nema_obj_init(&I.obj, &IN_OPS);
}

nema_obj_t *pal_input_obj(void) { return &I.obj; }
bool pal_input_nm_latched(void) { return I.nm_latch || I.nm > 0; }

static void push(const pal_key_t *k) {
    pal_perf_key_in((uint32_t)k->sym);
    nema_port_mutex_lock(I.lock);
    if (I.count < QUEUE_LEN) {
        I.q[(I.head + I.count) % QUEUE_LEN] = *k;
        I.count++;
    }
    nema_port_mutex_unlock(I.lock);
    nema_notify(&I.obj);
}

void pal_input_inject(const pal_key_t *k) { push(k); }

bool pal_input_pop(pal_key_t *out) {
    bool ok = false;
    nema_port_mutex_lock(I.lock);
    if (I.count > 0) {
        *out = I.q[I.head];
        I.head = (I.head + 1) % QUEUE_LEN;
        I.count--;
        ok = true;
    }
    nema_port_mutex_unlock(I.lock);
    return ok;
}

static void modes_changed(void) { pal_status_changed(); }

bool pal_input_mode(pal_input_mode_t m) { return m >= 0 && m < PAL_INPUT_MODES && I.mode[m]; }

void pal_input_set_mode(pal_input_mode_t m, bool on) {
    if (m < 0 || m >= PAL_INPUT_MODES || I.mode[m] == on) return;
    I.mode[m] = on;
    if (on && m == PAL_INPUT_SYM) I.mode[PAL_INPUT_NUM] = false;
    if (on && m == PAL_INPUT_NUM) I.mode[PAL_INPUT_SYM] = false;
    modes_changed();
}

static uint8_t mods_now(void) {
    return (uint8_t)((I.shift ? PAL_MOD_SHIFT : 0) | (I.ctrl || I.mode[PAL_INPUT_CTRL] ? PAL_MOD_CTRL : 0) | (I.alt ? PAL_MOD_ALT : 0));
}

/* Resolve a character key through the layers. */
static uint32_t layer_char(const pal_physkey_t *pk) {
    bool sym = (I.nm > 0 || I.nm_latch) != I.mode[PAL_INPUT_SYM]; /* Nm held or tapped flips a locked layer back */
    if (I.mode[PAL_INPUT_NUM] && !(I.nm > 0 || I.nm_latch) && pk->sym >= '0' && pk->sym <= '9') return pk->sym;
    if (sym) {
        if (I.shift && pk->alt) return pk->alt;
        if (pk->sym) return pk->sym;
        if (pk->alt && I.shift) return pk->alt;
    }
    uint8_t c = pk->base;
    if ((I.shift != 0) != I.mode[PAL_INPUT_CAPS] && c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
    return c;
}

void pal_input_set_clock(uint32_t (*now_ms)(void)) { I.clock = now_ms; }
uint32_t pal_input_real_clock(void) { return (uint32_t)(nema_port_time_us() / 1000u); }
static uint32_t stamp(void) { return I.clock ? I.clock() : I.last_ms; }
uint8_t pal_input_mods_held(void) { return mods_now(); }

/* Shift released: two quick taps of it, with nothing pressed in between, turn Caps Lock on or off. */
static void shift_released(void) {
    uint32_t t = stamp();
    if (!I.shift_clean || t - I.shift_down_ms >= FN_TAP_MAX_MS) {
        I.shift_tap_pending = false;
        return;
    }
    if (I.shift_tap_pending && I.shift_down_ms - I.shift_tap_up_ms < FN_GAP_MAX_MS) {
        I.shift_tap_pending = false;
        pal_input_set_mode(PAL_INPUT_CAPS, !I.mode[PAL_INPUT_CAPS]);
        return;
    }
    I.shift_tap_pending = true;
    I.shift_tap_up_ms = t;
}

/* Fn released: count it as a tap when nothing else was pressed meanwhile; the second quick tap emits PAL_KEY_VKB. */
static void fn_released(void) {
    uint32_t t = stamp();
    if (!I.fn_clean || t - I.fn_down_ms >= FN_TAP_MAX_MS) {
        I.fn_tap_pending = false;
        return;
    }
    if (I.fn_tap_pending && I.fn_down_ms - I.fn_tap_up_ms < FN_GAP_MAX_MS) {
        I.fn_tap_pending = false; /* the third tap starts a new count: a triple tap opens the keyboard once */
        pal_key_t k = {PAL_KEY_VKB, 0, false, 0};
        push(&k);
        return;
    }
    I.fn_tap_pending = true;
    I.fn_tap_up_ms = t;
    if (I.fn_tap_report) { /* a single tap that may still become a double one: the menu moves down now, and closes on the second */
        pal_key_t k = {PAL_KEY_FNTAP, 0, false, 0};
        push(&k);
    }
}

void pal_input_report_fn_tap(bool on) { I.fn_tap_report = on; }

void pal_input_hw(int row, int col, bool pressed) {
    if (!I.kb || row < 0 || col < 0 || row >= I.kb->rows || col >= I.kb->cols) return;
    const pal_physkey_t *pk = &I.kb->keys[row * I.kb->cols + col];
    int d = pressed ? 1 : -1;
    if (pressed && pk->kind != PAL_PK_FN && I.fn > 0) I.fn_clean = false; /* Fn+key is not a tap of Fn */
    if (pressed && pk->kind != PAL_PK_SHIFT) I.shift_clean = false;        /* Shift+key is not a tap of Shift */
    switch (pk->kind) {
    case PAL_PK_SHIFT:
        if (pressed && I.shift == 0) {
            I.shift_clean = true;
            I.shift_down_ms = stamp();
        }
        I.shift += d;
        if (I.shift < 0) I.shift = 0;
        if (!pressed && I.shift == 0) shift_released();
        return;
    case PAL_PK_CTRL: I.ctrl += d; if (I.ctrl < 0) I.ctrl = 0; return;
    case PAL_PK_ALT: I.alt += d; if (I.alt < 0) I.alt = 0; return;
    case PAL_PK_FN:
        if (pressed) {
            if (I.fn == 0) {
                I.fn_clean = true;
                I.fn_down_ms = stamp();
            }
            I.fn++;
            return;
        }
        if (I.fn > 0) I.fn--;
        if (I.fn == 0) fn_released();
        return;
    case PAL_PK_NM:
        if (pressed) {
            I.nm++;
            I.nm_used = false;
        } else {
            if (I.nm > 0) I.nm--;
            /* a tap latches the layer for the next character; using it while held does not */
            if (I.nm == 0 && !I.nm_used) I.nm_latch = !I.nm_latch;
        }
        return;
    case PAL_PK_CYCLE:
        if (pressed) {
            I.cycle_down = true;
            I.cycle_fired = false;
            I.cycle_at_ms = stamp();
        } else {
            if (I.cycle_down && !I.cycle_fired) {
                pal_key_t k = {PAL_KEY_SWITCH, 0, false, 0};
                push(&k);
            }
            I.cycle_down = false;
        }
        return;
    default: break;
    }
    if (!pressed) {
        if (I.rep_pk == pk) I.rep_active = false;
        return;
    }
    if (I.nm > 0) I.nm_used = true;

    pal_key_t k = {PAL_KEY_NONE, mods_now(), false, 0};
    if (I.fn && pk->fn) {
        k.sym = pk->fn;
    } else if (pk->kind == PAL_PK_SPECIAL) {
        k.sym = pk->base;
    } else if (pk->kind == PAL_PK_CHAR) {
        k.sym = PAL_KEY_CHAR;
        k.cp = layer_char(pk);
        if (I.nm_latch && I.nm == 0) I.nm_latch = false; /* one-shot consumed */
    } else {
        return;
    }
    if (k.sym == PAL_KEY_NONE) return;
    if (I.mode[PAL_INPUT_CTRL]) { /* Ctrl for the next key only: it has gone into k.mods above, and it does not repeat */
        I.mode[PAL_INPUT_CTRL] = false;
        modes_changed();
        push(&k);
        return;
    }
    push(&k);
    I.rep_active = true;
    I.rep_pk = pk;
    I.rep_key = k;
    I.rep_key.repeat = true;
    I.rep_next_ms = stamp() + s_repeat_delay;
}

void pal_input_tick(uint32_t now_ms) {
    I.last_ms = now_ms;
    if (I.cycle_down && !I.cycle_fired && (int32_t)(now_ms - I.cycle_at_ms) >= LONG_PRESS_MS) {
        I.cycle_fired = true;
        pal_key_t k = {PAL_KEY_GOHOME, 0, false, 0};
        push(&k);
    }
    if (I.rep_active && (int32_t)(now_ms - I.rep_next_ms) >= 0) {
        push(&I.rep_key);
        I.rep_next_ms = now_ms + s_repeat_rate;
    }
}

int32_t pal_input_next_tick_ms(uint32_t now_ms) {
    int32_t best = -1;
    if (I.rep_active) {
        int32_t d = (int32_t)(I.rep_next_ms - now_ms);
        best = d < 0 ? 0 : d;
    }
    if (I.cycle_down && !I.cycle_fired) {
        int32_t d = (int32_t)(I.cycle_at_ms + LONG_PRESS_MS - now_ms);
        d = d < 0 ? 0 : d;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

size_t pal_key_to_bytes(const pal_key_t *k, bool app_cursor, char *out, size_t cap) {
    char tmp[12];
    size_t n = 0;
    const char *csi = app_cursor ? "\x1bO" : "\x1b[";
    switch (k->sym) {
    case PAL_KEY_CHAR: {
        uint32_t cp = k->cp;
        if ((k->mods & PAL_MOD_CTRL) && cp < 0x80) {
            if (cp >= 'a' && cp <= 'z') cp = cp - 'a' + 1;
            else if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 1;
            else if (cp == '@' || cp == ' ') cp = 0;
            else if (cp == '[') cp = 0x1B;
            else if (cp == '\\') cp = 0x1C;
            else if (cp == ']') cp = 0x1D;
            else if (cp == '^') cp = 0x1E;
            else if (cp == '_' || cp == '-') cp = 0x1F;
            else if (cp == '?') cp = 0x7F;
        }
        if (k->mods & PAL_MOD_ALT) tmp[n++] = 0x1B;
        if (cp < 0x80) tmp[n++] = (char)cp;
        else if (cp < 0x800) { tmp[n++] = (char)(0xC0 | (cp >> 6)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
        else { tmp[n++] = (char)(0xE0 | (cp >> 12)); tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
        break;
    }
    case PAL_KEY_ENTER: tmp[n++] = '\r'; break;
    case PAL_KEY_ESC: tmp[n++] = 0x1B; break;
    case PAL_KEY_BACKSPACE: tmp[n++] = 0x7F; break;
    case PAL_KEY_TAB: tmp[n++] = '\t'; break;
    case PAL_KEY_UP: n = (size_t)snprintf(tmp, sizeof(tmp), "%sA", csi); break;
    case PAL_KEY_DOWN: n = (size_t)snprintf(tmp, sizeof(tmp), "%sB", csi); break;
    case PAL_KEY_RIGHT: n = (size_t)snprintf(tmp, sizeof(tmp), "%sC", csi); break;
    case PAL_KEY_LEFT: n = (size_t)snprintf(tmp, sizeof(tmp), "%sD", csi); break;
    case PAL_KEY_HOME: n = (size_t)snprintf(tmp, sizeof(tmp), "%sH", csi); break;
    case PAL_KEY_END: n = (size_t)snprintf(tmp, sizeof(tmp), "%sF", csi); break;
    case PAL_KEY_PGUP: n = (size_t)snprintf(tmp, sizeof(tmp), "\x1b[5~"); break;
    case PAL_KEY_PGDN: n = (size_t)snprintf(tmp, sizeof(tmp), "\x1b[6~"); break;
    case PAL_KEY_DELETE: n = (size_t)snprintf(tmp, sizeof(tmp), "\x1b[3~"); break;
    case PAL_KEY_INSERT: n = (size_t)snprintf(tmp, sizeof(tmp), "\x1b[2~"); break;
    default: return 0;
    }
    if (n > cap) n = cap;
    memcpy(out, tmp, n);
    return n;
}
