/* Key layout of the hhv2 board: a 4x10 matrix scanned by a TCA8418, 39 keys.
 * Measured on the real board (see docs/KEYMAP.md). The matrix columns run right to left, so C9 is the
 * leftmost key. Pure data: used by the firmware and by the host simulator.
 *
 * Layers:
 *   base      the letters, '.', space, and the named keys
 *   Nm        the symbols printed on the keys (hold Nm, or tap it to latch for one key)
 *   Nm+Shift  the remaining ASCII symbols, arranged on the first keys of the top rows
 *   Fn        navigation: W A S D = arrows, Q/E = Home/End, R/F = PgUp/PgDn, del = Delete
 */
#include "pal/input.h"

#define CH(b, s, a, f) {PAL_PK_CHAR, b, s, a, f}
#define SP(k, f) {PAL_PK_SPECIAL, k, 0, 0, f}
#define MOD(k) {k, 0, 0, 0, 0}
#define NONE {PAL_PK_NONE, 0, 0, 0, 0}

static const pal_physkey_t KEYS[4 * 10] = {
    /* R0: C0..C9 = P O I U Y T R E W Q */
    CH('p', '@', '~', 0), CH('o', '+', '\\', 0), CH('i', '-', '|', 0), CH('u', '_', '=', 0),
    CH('y', ')', '>', 0), CH('t', '(', '<', 0),  CH('r', '3', '}', PAL_KEY_PGUP), CH('e', '2', '{', PAL_KEY_END),
    CH('w', '1', ']', PAL_KEY_UP), CH('q', '#', '[', PAL_KEY_HOME),
    /* R1: del L K J H G F D S A */
    SP(PAL_KEY_BACKSPACE, PAL_KEY_DELETE), CH('l', '"', 0, 0), CH('k', '\'', 0, 0), CH('j', ';', 0, 0),
    CH('h', ':', 0, 0), CH('g', '/', 0, 0), CH('f', '6', 0, PAL_KEY_PGDN), CH('d', '5', '`', PAL_KEY_RIGHT),
    CH('s', '4', '^', PAL_KEY_DOWN), CH('a', '*', '&', PAL_KEY_LEFT),
    /* R2: enter . M N B V C X Z shift */
    SP(PAL_KEY_ENTER, 0), CH('.', '$', 0, 0), CH('m', ',', 0, 0), CH('n', '!', 0, 0), CH('b', '?', 0, 0),
    CH('v', '%', 0, 0), CH('c', '9', 0, 0), CH('x', '8', 0, 0), CH('z', '7', 0, 0), MOD(PAL_PK_SHIFT),
    /* R3: alt shift ctrl tab space (none) cycle Nm fn esc */
    MOD(PAL_PK_ALT), MOD(PAL_PK_SHIFT), MOD(PAL_PK_CTRL), SP(PAL_KEY_TAB, 0), CH(' ', '0', 0, 0), NONE,
    MOD(PAL_PK_CYCLE), MOD(PAL_PK_NM), MOD(PAL_PK_FN), SP(PAL_KEY_ESC, 0),
};

const pal_keyboard_t HHV2_KEYBOARD = {4, 10, KEYS};
