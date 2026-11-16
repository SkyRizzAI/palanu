// Map of the TCA8418 matrix -> physical keys of the hhv2 keyboard.
// Measured on 12 Nov 2026 with 02_keyboard: the keys were pressed in order,
// left to right, top row to bottom, then matched against the serial events.
//
// Findings:
//   - Matrix rows R0..R3 = physical rows top..bottom.
//   - Columns are REVERSED: the leftmost physical key = C9, the rightmost = C0.
//   - R3 C5 is not fitted (39 keys in 40 positions).
//   - TCA8418 event code = row*10 + col + 1.
#pragma once
#include <stdint.h>

#define HHV2_KB_ROWS 4
#define HHV2_KB_COLS 10

typedef enum {
    HHV2_KEY_NONE = 0,
    // letters and digits use their upper-case ASCII code
    HHV2_KEY_DEL = 0x80,
    HHV2_KEY_SHIFT_L,
    HHV2_KEY_SHIFT_R,
    HHV2_KEY_ENTER,
    HHV2_KEY_ESC,
    HHV2_KEY_FN,
    HHV2_KEY_NUM,     // "Nm" (secondary: 0)
    HHV2_KEY_CYCLE,   // circular arrow icon; its function is not decided yet
    HHV2_KEY_SPACE,
    HHV2_KEY_TAB,
    HHV2_KEY_CTRL,
    HHV2_KEY_ALT,
} hhv2_key_t;

// Index [row][col] of the matrix, not the physical position
static const uint16_t HHV2_KEYMAP[HHV2_KB_ROWS][HHV2_KB_COLS] = {
    //  C0               C1             C2              C3            C4              C5              C6              C7            C8           C9
    {'P',              'O',           'I',            'U',          'Y',            'T',            'R',            'E',          'W',         'Q'},
    {HHV2_KEY_DEL,     'L',           'K',            'J',          'H',            'G',            'F',            'D',          'S',         'A'},
    {HHV2_KEY_ENTER,   '.',           'M',            'N',          'B',            'V',            'C',            'X',          'Z',         HHV2_KEY_SHIFT_L},
    {HHV2_KEY_ALT,     HHV2_KEY_SHIFT_R, HHV2_KEY_CTRL, HHV2_KEY_TAB, HHV2_KEY_SPACE, HHV2_KEY_NONE, HHV2_KEY_CYCLE, HHV2_KEY_NUM, HHV2_KEY_FN, HHV2_KEY_ESC},
};

// Short label (4 characters at most) for display and logs
static inline const char *hhv2_key_label(uint16_t k)
{
    static char one[2];
    switch (k) {
    case HHV2_KEY_NONE: return "";
    case HHV2_KEY_DEL: return "DEL";
    case HHV2_KEY_SHIFT_L: return "SHFL";
    case HHV2_KEY_SHIFT_R: return "SHFR";
    case HHV2_KEY_ENTER: return "ENT";
    case HHV2_KEY_ESC: return "ESC";
    case HHV2_KEY_FN: return "FN";
    case HHV2_KEY_NUM: return "NM";
    case HHV2_KEY_CYCLE: return "CYC";
    case HHV2_KEY_SPACE: return "SPC";
    case HHV2_KEY_TAB: return "TAB";
    case HHV2_KEY_CTRL: return "CTRL";
    case HHV2_KEY_ALT: return "ALT";
    default:
        one[0] = (char)k;
        one[1] = 0;
        return one;
    }
}
