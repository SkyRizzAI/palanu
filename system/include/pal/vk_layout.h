/* PalanuOS on-screen keyboard: the pure part. The key tables for each layer, focus movement on the grid, the shift and
 * sticky-modifier rules, the translation of a key into the pal_key_t the physical keyboard would send, and the text
 * helpers. Nothing here draws or touches the desktop, so the rules are tested on the host (tests/unit/test_vk.c).
 *
 * Geometry: a row is 20 half-units wide; a key takes w half-units (2 = one unit). Every row sums to 20, so the keys of
 * different rows line up on the same grid. */
#ifndef PAL_VK_LAYOUT_H
#define PAL_VK_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "pal/input.h"

#define VK_ROW_HU 20
#define VK_ROWS 4
#define VK_MAX_CELLS 48

/* Layers. CTL is not reachable in entry mode. */
enum { VKL_LETTERS = 0, VKL_SYM, VKL_SYM2, VKL_CTL };
/* Shift: OFF; ONE = latched for one letter (the ABC layer); CAPS = locked (double tap). */
enum { VK_SHIFT_OFF = 0, VK_SHIFT_ONE, VK_SHIFT_CAPS };
#define VK_MOD_CTRL 0x2u /* same bits as PAL_MOD_CTRL / PAL_MOD_ALT */
#define VK_MOD_ALT 0x4u

typedef enum {
    VK_K_CHAR,    /* cp; letters follow the shift state */
    VK_K_CHORD,   /* Ctrl + cp (the ^C keys) */
    VK_K_BACK,    /* Backspace */
    VK_K_ENTER,   /* Enter, sent to the app */
    VK_K_OK,      /* entry mode: confirm */
    VK_K_SPECIAL, /* sym: a pal_keysym_t (Tab, Esc, arrows, Home, ...) */
    VK_K_LAYER,   /* switch to layer */
    VK_K_SHIFT,   /* the shift cap */
    VK_K_MOD,     /* sticky Ctrl or Alt: mod holds the VK_MOD_* bit */
    VK_K_SHOW,    /* entry mode with mask: reveal or hide the text */
    VK_K_CANCEL,  /* entry mode: the X key, cancels the entry (the reference row 3) */
} vk_kind_t;

typedef struct {
    uint8_t kind;  /* vk_kind_t */
    uint8_t w;     /* half-units */
    uint8_t layer; /* VK_K_LAYER target */
    uint8_t mod;   /* VK_K_MOD bit */
    uint8_t sym;   /* VK_K_SPECIAL: pal_keysym_t */
    uint32_t cp;   /* VK_K_CHAR / VK_K_CHORD */
    const char *label; /* text label; NULL: drawn from the kind (or the character) */
} vk_key_t;

typedef struct {
    uint8_t layer, shift, sticky; /* sticky: VK_MOD_* bits latched for the next key */
    uint8_t entry, mask, show;    /* entry mode, masked text, text shown while masked */
    uint32_t shift_tap_ms;        /* time of the last shift tap (double tap detection) */
} vk_ui_t;

typedef struct {
    uint8_t row, x, w; /* half-units */
    const vk_key_t *key;
} vk_cell_t;

typedef enum { VK_DIR_LEFT, VK_DIR_RIGHT, VK_DIR_UP, VK_DIR_DOWN, VK_DIR_HOME, VK_DIR_END, VK_DIR_PGUP, VK_DIR_PGDN } vk_dir_t;

/* The cells of the current layer (rows top to bottom, left to right). Returns the count, or -1 when cap is too small. */
int vk_build(const vk_ui_t *u, vk_cell_t *out, int cap);
/* The cell to start on, and to come back to after a layer switch. Entry mode (a password, a name) keeps the reference's: the middle of the
 * second row. The live keyboard (Fn Fn, Keyboard) starts on the layer key at the left of the bottom row instead (123, abc, #+=),
 * so changing to symbols or digits needs no travel across the keys. */
int vk_focus_home(const vk_cell_t *c, int n, bool entry);
/* Moves the focus. pref2 is the column to return to (twice the centre, in half-units): horizontal moves set it, vertical
 * moves keep it. Wraps at the edges of a row and between the top and bottom rows. */
int vk_move(const vk_cell_t *c, int n, int focus, vk_dir_t d, int *pref2);
int vk_pref2(const vk_cell_t *c, int focus);

/* The shift cap: OFF -> ONE; a second tap within 400 ms -> CAPS; any other tap -> OFF. */
void vk_shift_tap(vk_ui_t *u, uint32_t now_ms);
/* Tab: letters -> sym -> sym2 -> ctl -> letters (entry mode skips ctl). */
void vk_cycle_layer(vk_ui_t *u);
/* The upper-case form of a letter; other code points are returned unchanged. */
uint32_t vk_upper(uint32_t cp);
/* Whether a character key shows and sends its upper-case form. */
int vk_is_upper(const vk_ui_t *u, uint8_t phys_mods);

/* The event the physical keyboard would send for this key. Returns 0 for keys that only change the keyboard (shift,
 * layers, sticky modifiers, show). upper: the shift state for letters. */
int vk_key_event(const vk_key_t *k, int upper, pal_key_t *out);

/* Text in a fixed buffer (NUL-terminated, cap bytes). */
size_t vk_utf8_put(char *out, uint32_t cp); /* 1..4 bytes */
size_t vk_text_insert(char *buf, size_t len, size_t cap, uint32_t cp); /* at the end; returns the new length */
size_t vk_text_backspace(char *buf, size_t len);                       /* removes the last code point */
size_t vk_cp_count(const char *s, size_t len);                         /* code points in s[0..len) */
size_t vk_cp_index(const char *s, size_t len, size_t off);             /* code points before byte off */
/* Caret scrolling in code points: the new first visible column, keeping caret on the vis columns. */
size_t vk_scroll_for(size_t scroll, size_t caret, size_t vis);

#endif
