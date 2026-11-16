/* PalanuOS input: physical keys in, logical key events out.
 *
 * The board describes its keys (see pal_physkey_t); this service owns the modifier state machine, the
 * layers (the printed secondary symbols, extras, Fn navigation), key repeat, and the global gesture on the
 * cycle key (tap = switch app, hold = home). Consumers read pal_key_t events from one queue.
 */
#ifndef PAL_INPUT_H
#define PAL_INPUT_H

#include <stdint.h>

#include "nema/obj.h"

typedef enum {
    PAL_KEY_NONE = 0,
    PAL_KEY_CHAR, /* a printable code point in cp */
    PAL_KEY_ENTER, PAL_KEY_ESC, PAL_KEY_BACKSPACE, PAL_KEY_TAB, PAL_KEY_DELETE,
    PAL_KEY_UP, PAL_KEY_DOWN, PAL_KEY_LEFT, PAL_KEY_RIGHT,
    PAL_KEY_HOME, PAL_KEY_END, PAL_KEY_PGUP, PAL_KEY_PGDN,
    PAL_KEY_SWITCH, /* tap on the cycle key */
    PAL_KEY_GOHOME, /* long press on the cycle key */
    PAL_KEY_VKB,    /* Fn tapped twice quickly: the on-screen keyboard (vk.h) */
    PAL_KEY_INSERT, /* on-screen keyboard only; no physical key makes it */
    PAL_KEY_FNTAP,  /* one clean tap of Fn, sent only while pal_input_report_fn_tap(true): the input menu's Down */
} pal_keysym_t;

#define PAL_MOD_SHIFT 0x1u
#define PAL_MOD_CTRL 0x2u
#define PAL_MOD_ALT 0x4u

typedef struct {
    uint8_t sym; /* pal_keysym_t */
    uint8_t mods;
    bool repeat;
    uint32_t cp; /* for PAL_KEY_CHAR */
} pal_key_t;

/* ---- what a board provides for each physical key ---- */
typedef enum {
    PAL_PK_NONE = 0,
    PAL_PK_CHAR,    /* letter or symbol key: base, sym (Nm layer), alt (Nm+Shift layer) */
    PAL_PK_SPECIAL, /* base holds a pal_keysym_t */
    PAL_PK_SHIFT, PAL_PK_CTRL, PAL_PK_ALT, PAL_PK_FN, PAL_PK_NM, PAL_PK_CYCLE,
} pal_physkind_t;

typedef struct {
    uint8_t kind;
    uint8_t base, sym, alt; /* ASCII; 0 = none */
    uint8_t fn;             /* pal_keysym_t produced with Fn held; 0 = none */
} pal_physkey_t;

typedef struct {
    int rows, cols;
    const pal_physkey_t *keys; /* rows * cols entries */
} pal_keyboard_t;

void pal_input_init(const pal_keyboard_t *kb);
/* Hardware event from the board: position and state. Safe from any task. */
void pal_input_hw(int row, int col, bool pressed);
/* Called periodically with a millisecond clock; produces key repeats and the long-press gesture. */
void pal_input_tick(uint32_t now_ms);
/* The clock used to stamp a key press (when its repeat and long-press timers start). Key events arrive from the keyboard
 * task between desktop ticks, and the desktop sleeps up to a second when idle, so stamping a press with the time of the
 * LAST TICK made the first repeat fire at once ("h" typed three times) and a short tap of the cycle key count as a long
 * press. The firmware passes pal_input_real_clock; tests pass a fake one; NULL means "time of the last tick". */
void pal_input_set_clock(uint32_t (*now_ms)(void));
uint32_t pal_input_real_clock(void);
/* Milliseconds until pal_input_tick needs to run again, or -1 if nothing is pending. */
int32_t pal_input_next_tick_ms(uint32_t now_ms);

bool pal_input_pop(pal_key_t *out);
nema_obj_t *pal_input_obj(void); /* IN when pal_input_pop would succeed */
void pal_input_inject(const pal_key_t *k); /* tests, remote input */
bool pal_input_nm_latched(void);

/* While on, every clean tap of Fn (short, with no other key pressed while it was held) also sends PAL_KEY_FNTAP, at the moment Fn
 * is released; the second tap of a quick pair sends PAL_KEY_VKB instead. The input menu turns it on while it is open. Any task. */
void pal_input_report_fn_tap(bool on);

/* Sticky modes of the physical keyboard, set from the input menu (Fn double tap) or by tapping Shift twice (Caps). CAPS makes the
 * letters upper case until it is turned off (Shift held inverts it); SYM keeps the Nm layer on, so the letter keys give their
 * printed symbols (holding Nm gives the letters back); NUM is the same only for the digits (W E R S D F Z X C and space as
 * 1-9 and 0) while the other keys stay letters; CTRL applies to the next key only. SYM and NUM exclude each other. Any task. */
typedef enum { PAL_INPUT_CAPS, PAL_INPUT_SYM, PAL_INPUT_NUM, PAL_INPUT_CTRL, PAL_INPUT_MODES } pal_input_mode_t;
bool pal_input_mode(pal_input_mode_t m);
void pal_input_set_mode(pal_input_mode_t m, bool on);
/* Key repeat (Settings: input.repeat_delay, input.repeat_rate): the delay before the first repeat and the gap between repeats,
 * in milliseconds. Out-of-range values are ignored. The long-press time of the cycle key is fixed. */
void pal_input_set_repeat(uint32_t delay_ms, uint32_t rate_ms);
void pal_input_get_repeat(uint32_t *delay_ms, uint32_t *rate_ms);
uint32_t pal_input_long_press_ms(void);
/* Shift, Ctrl and Alt (PAL_MOD_*) held on the physical keyboard right now. */
uint8_t pal_input_mods_held(void);

/* Encode a key as terminal input bytes. Returns the length (0 if the key produces none). */
size_t pal_key_to_bytes(const pal_key_t *k, bool app_cursor, char *out, size_t cap);

#endif
