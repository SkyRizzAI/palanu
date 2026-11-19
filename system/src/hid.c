/* PalanuOS keys to USB HID keyboard usages (pal/hid.h). The table follows the US layout, which is what a host assumes of a keyboard
 * that does not say otherwise: a character is its key plus Shift when the US layout puts it on the shifted key. */
#include "pal/hid.h"

#include <string.h>

static const pal_hid_ops_t *s_ops;

void pal_hid_set(const pal_hid_ops_t *ops) { s_ops = ops; }
const pal_hid_ops_t *pal_hid(void) { return s_ops; }

/* ASCII 0x20..0x7E: the usage, and 0x80 when Shift is needed. */
#define S 0x80u
static const uint8_t ASCII[95] = {
    0x2C,     0x1E | S, 0x34 | S, 0x20 | S, 0x21 | S, 0x22 | S, 0x24 | S, 0x34, /* space ! " # $ % & ' */
    0x26 | S, 0x27 | S, 0x25 | S, 0x2E | S, 0x36,     0x2D,     0x37,     0x38, /* ( ) * + , - . / */
    0x27,     0x1E,     0x1F,     0x20,     0x21,     0x22,     0x23,     0x24, /* 0..7 */
    0x25,     0x26,     0x33 | S, 0x33,     0x36 | S, 0x2E,     0x37 | S, 0x38 | S, /* 8 9 : ; < = > ? */
    0x1F | S,                                                                  /* @ */
    0x04 | S, 0x05 | S, 0x06 | S, 0x07 | S, 0x08 | S, 0x09 | S, 0x0A | S, 0x0B | S, 0x0C | S, 0x0D | S, 0x0E | S, 0x0F | S, 0x10 | S,
    0x11 | S, 0x12 | S, 0x13 | S, 0x14 | S, 0x15 | S, 0x16 | S, 0x17 | S, 0x18 | S, 0x19 | S, 0x1A | S, 0x1B | S, 0x1C | S, 0x1D | S, /* A..Z */
    0x2F,     0x31,     0x30,     0x23 | S, 0x2D | S, 0x35,                     /* [ \ ] ^ _ ` */
    0x04,     0x05,     0x06,     0x07,     0x08,     0x09,     0x0A,     0x0B,     0x0C,     0x0D,     0x0E,     0x0F,     0x10,
    0x11,     0x12,     0x13,     0x14,     0x15,     0x16,     0x17,     0x18,     0x19,     0x1A,     0x1B,     0x1C,     0x1D, /* a..z */
    0x2F | S, 0x31 | S, 0x30 | S, 0x35 | S,                                     /* { | } ~ */
};
#undef S

bool pal_hid_from_key(const pal_key_t *k, uint8_t *usage, uint8_t *mods) {
    uint8_t u = 0, m = 0;
    switch (k->sym) {
    case PAL_KEY_CHAR:
        if (k->cp < 0x20 || k->cp > 0x7E) return false;
        u = ASCII[k->cp - 0x20] & 0x7Fu;
        if (ASCII[k->cp - 0x20] & 0x80u) m |= PAL_HID_MOD_LSHIFT;
        break;
    case PAL_KEY_ENTER: u = 0x28; break;
    case PAL_KEY_ESC: u = 0x29; break;
    case PAL_KEY_BACKSPACE: u = 0x2A; break;
    case PAL_KEY_TAB: u = 0x2B; break;
    case PAL_KEY_DELETE: u = 0x4C; break;
    case PAL_KEY_INSERT: u = 0x49; break;
    case PAL_KEY_HOME: u = 0x4A; break;
    case PAL_KEY_PGUP: u = 0x4B; break;
    case PAL_KEY_END: u = 0x4D; break;
    case PAL_KEY_PGDN: u = 0x4E; break;
    case PAL_KEY_RIGHT: u = 0x4F; break;
    case PAL_KEY_LEFT: u = 0x50; break;
    case PAL_KEY_DOWN: u = 0x51; break;
    case PAL_KEY_UP: u = 0x52; break;
    default: return false;
    }
    /* the modifiers held: Shift on a character is already in the character (A, !), so only Ctrl and Alt are added there */
    if (k->mods & PAL_MOD_CTRL) m |= PAL_HID_MOD_LCTRL;
    if (k->mods & PAL_MOD_ALT) m |= PAL_HID_MOD_LALT;
    if ((k->mods & PAL_MOD_SHIFT) && k->sym != PAL_KEY_CHAR) m |= PAL_HID_MOD_LSHIFT;
    *usage = u;
    *mods = m;
    return true;
}

nema_err_t pal_hid_type(const pal_key_t *k) {
    uint8_t u, m;
    if (!pal_hid_from_key(k, &u, &m)) return NEMA_ERR_INVAL;
    if (!s_ops || !s_ops->state || s_ops->state() != PAL_HID_CONNECTED) return NEMA_ERR_STATE;
    uint8_t keys[6] = {u, 0, 0, 0, 0, 0};
    nema_err_t r = s_ops->report(m, keys);
    memset(keys, 0, sizeof(keys));
    nema_err_t r2 = s_ops->report(0, keys); /* released: the host repeats a held key itself, so every press is let go */
    return r != NEMA_OK ? r : r2;
}
