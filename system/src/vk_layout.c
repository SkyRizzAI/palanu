/* The pure part of the on-screen keyboard (see pal/vk_layout.h): the key tables, focus movement, shift rules, key
 * translation and text helpers. The key tables are the layers of docs/VKEYBOARD.md. */
#include <string.h>

#include "pal/vk_layout.h"

#define CH(c, w) {VK_K_CHAR, (w), 0, 0, 0, (c), NULL}
#define CHL(c, w, lab) {VK_K_CHAR, (w), 0, 0, 0, (c), (lab)}
#define LAY(l, w, lab) {VK_K_LAYER, (w), (l), 0, 0, 0, (lab)}
#define KIND(k, w, lab) {(k), (w), 0, 0, 0, 0, (lab)}
#define SPC(s, w, lab) {VK_K_SPECIAL, (w), 0, 0, (s), 0, (lab)}
#define MOD(m, w, lab) {VK_K_MOD, (w), 0, (m), 0, 0, (lab)}
#define CHORD(c, w, lab) {VK_K_CHORD, (w), 0, 0, 0, (c), (lab)}

/* ---- letters (abc, and ABC when shift is on) ---- */
static const vk_key_t LET_R0[] = {CH('q', 2), CH('w', 2), CH('e', 2), CH('r', 2), CH('t', 2), CH('y', 2), CH('u', 2), CH('i', 2), CH('o', 2), CH('p', 2)};
static const vk_key_t LET_R1[] = {CH('a', 2), CH('s', 2), CH('d', 2), CH('f', 2), CH('g', 2), CH('h', 2), CH('j', 2), CH('k', 2), CH('l', 2), CH('\'', 2)};
static const vk_key_t LET_R2[] = {KIND(VK_K_SHIFT, 3, NULL), CH('z', 2), CH('x', 2), CH('c', 2), CH('v', 2), CH('b', 2), CH('n', 2), CH('m', 2), KIND(VK_K_BACK, 3, NULL)};
static const vk_key_t LET_R3[] = {LAY(VKL_SYM, 3, "123"), CHL(',', 2, NULL), CHL(' ', 8, "space"), CH('.', 2), KIND(VK_K_ENTER, 5, NULL)};
static const vk_key_t LET_R3_MASK[] = {LAY(VKL_SYM, 3, "123"), CHL(',', 2, NULL), CHL(' ', 6, "space"), CH('.', 2), KIND(VK_K_SHOW, 2, NULL), KIND(VK_K_OK, 5, "OK")};
static const vk_key_t LET_R3_PLAIN[] = {LAY(VKL_SYM, 3, "123"), CHL(',', 2, NULL), CHL(' ', 8, "space"), CH('.', 2), KIND(VK_K_OK, 5, "OK")};

/* ---- 123 ---- */
static const vk_key_t SYM_R0[] = {CH('1', 2), CH('2', 2), CH('3', 2), CH('4', 2), CH('5', 2), CH('6', 2), CH('7', 2), CH('8', 2), CH('9', 2), CH('0', 2)};
static const vk_key_t SYM_R1[] = {CH('-', 2), CH('/', 2), CH(':', 2), CH(';', 2), CH('(', 2), CH(')', 2), CH('$', 2), CH('&', 2), CH('@', 2), CH('"', 2)};
static const vk_key_t SYM_R2[] = {CH('\'', 2), CH('!', 2), CH('?', 2), CH('+', 2), CH('*', 2), CH('%', 2), CH('=', 2), CH('_', 2), KIND(VK_K_BACK, 4, NULL)};
static const vk_key_t SYM_R3[] = {LAY(VKL_LETTERS, 3, "abc"), LAY(VKL_SYM2, 3, "#+="), CHL(',', 2, NULL), CHL(' ', 6, "space"), CH('.', 2), KIND(VK_K_ENTER, 4, NULL)};
static const vk_key_t SYM_R3_MASK[] = {LAY(VKL_LETTERS, 3, "abc"), LAY(VKL_SYM2, 3, "#+="), CHL(',', 2, NULL), CHL(' ', 4, "space"), CH('.', 2), KIND(VK_K_SHOW, 2, NULL), KIND(VK_K_OK, 4, "OK")};
static const vk_key_t SYM_R3_PLAIN[] = {LAY(VKL_LETTERS, 3, "abc"), LAY(VKL_SYM2, 3, "#+="), CHL(',', 2, NULL), CHL(' ', 6, "space"), CH('.', 2), KIND(VK_K_OK, 4, "OK")};

/* ---- #+= (the rest of ASCII punctuation, and shell characters) ---- */
static const vk_key_t SYM2_R0[] = {CH('#', 2), CH('_', 2), CH('=', 2), CH('~', 2), CH('`', 2), CH('|', 2), CH('\\', 2), CH('^', 2), CH('<', 2), CH('>', 2)};
static const vk_key_t SYM2_R1[] = {CH('[', 2), CH(']', 2), CH('{', 2), CH('}', 2), CH('&', 2), CH('(', 2), CH(')', 2), CH(';', 2), CH(':', 2), CH('\'', 2)};
static const vk_key_t SYM2_R2[] = {CH('$', 2), CH('/', 2), CH('*', 2), CH('?', 2), CH('!', 2), CH('~', 2), CH('|', 2), CH('\\', 2), KIND(VK_K_BACK, 4, NULL)};
static const vk_key_t SYM2_R3[] = {LAY(VKL_LETTERS, 3, "abc"), LAY(VKL_SYM, 3, "123"), LAY(VKL_CTL, 3, "ctl"), CHL(' ', 6, "space"), CH('.', 2), KIND(VK_K_ENTER, 3, NULL)};
static const vk_key_t SYM2_R3_MASK[] = {LAY(VKL_LETTERS, 3, "abc"), LAY(VKL_SYM, 3, "123"), CHL(',', 2, NULL), CHL(' ', 4, "space"), CH('.', 2), KIND(VK_K_SHOW, 2, NULL), KIND(VK_K_OK, 4, "OK")};
static const vk_key_t SYM2_R3_PLAIN[] = {LAY(VKL_LETTERS, 3, "abc"), LAY(VKL_SYM, 3, "123"), CHL(',', 2, NULL), CHL(' ', 6, "space"), CH('.', 2), KIND(VK_K_OK, 4, "OK")};

/* ---- ctl: keys a terminal needs that no printed key gives ---- */
static const vk_key_t CTL_R0[] = {SPC(PAL_KEY_ESC, 2, "Esc"), SPC(PAL_KEY_TAB, 2, "Tab"), MOD(VK_MOD_CTRL, 2, "Ctrl"), MOD(VK_MOD_ALT, 2, "Alt"),
                                  SPC(PAL_KEY_INSERT, 2, "Ins"), SPC(PAL_KEY_DELETE, 2, "Del"), SPC(PAL_KEY_HOME, 2, "Home"), SPC(PAL_KEY_END, 2, "End"),
                                  SPC(PAL_KEY_PGUP, 2, "PgUp"), SPC(PAL_KEY_PGDN, 2, "PgDn")};
static const vk_key_t CTL_R1[] = {SPC(PAL_KEY_LEFT, 5, NULL), SPC(PAL_KEY_UP, 5, NULL), SPC(PAL_KEY_DOWN, 5, NULL), SPC(PAL_KEY_RIGHT, 5, NULL)};
static const vk_key_t CTL_R2[] = {CHORD('c', 2, "^C"), CHORD('d', 2, "^D"), CHORD('z', 2, "^Z"), CHORD('l', 2, "^L"), CHORD('u', 2, "^U"),
                                  CHORD('w', 2, "^W"), CHORD('a', 2, "^A"), CHORD('e', 2, "^E"), KIND(VK_K_BACK, 4, NULL)};
static const vk_key_t CTL_R3[] = {LAY(VKL_LETTERS, 3, "abc"), LAY(VKL_SYM, 3, "123"), LAY(VKL_SYM2, 3, "#+="), CHL(' ', 6, "space"), CH('.', 2), KIND(VK_K_ENTER, 3, NULL)};

typedef struct {
    const vk_key_t *k;
    uint8_t n;
} vk_row_t;
#define ROW(a) {(a), (uint8_t)(sizeof(a) / sizeof((a)[0]))}

/* Rows 0..2 by layer, and row 3 by layer and entry variant (normal, entry with mask, entry without mask). */
static const vk_row_t BASE[4][3] = {
    {ROW(LET_R0), ROW(LET_R1), ROW(LET_R2)},
    {ROW(SYM_R0), ROW(SYM_R1), ROW(SYM_R2)},
    {ROW(SYM2_R0), ROW(SYM2_R1), ROW(SYM2_R2)},
    {ROW(CTL_R0), ROW(CTL_R1), ROW(CTL_R2)},
};
static const vk_row_t LAST[4][3] = {
    {ROW(LET_R3), ROW(LET_R3_MASK), ROW(LET_R3_PLAIN)},
    {ROW(SYM_R3), ROW(SYM_R3_MASK), ROW(SYM_R3_PLAIN)},
    {ROW(SYM2_R3), ROW(SYM2_R3_MASK), ROW(SYM2_R3_PLAIN)},
    {ROW(CTL_R3), ROW(CTL_R3), ROW(CTL_R3)},
};

/* ---- entry mode: the reference VirtualKeyboard grid. Ten keys a row;
 * row 1 ends in the DEL key, row 2 starts with the shift cap on the letters; the bottom row is MODE SPACE OK X. The layers
 * are the reference's: letters (Upper or Lower by shift), 123 (the reference Num) and #+= (the reference Sym). No ctl layer. */
static const vk_key_t E_LET_R0[] = {CH('q', 2), CH('w', 2), CH('e', 2), CH('r', 2), CH('t', 2), CH('y', 2), CH('u', 2), CH('i', 2), CH('o', 2), CH('p', 2)};
static const vk_key_t E_LET_R1[] = {CH('a', 2), CH('s', 2), CH('d', 2), CH('f', 2), CH('g', 2), CH('h', 2), CH('j', 2), CH('k', 2), CH('l', 2), KIND(VK_K_BACK, 2, "<-")};
static const vk_key_t E_LET_R2[] = {KIND(VK_K_SHIFT, 2, NULL), CH('z', 2), CH('x', 2), CH('c', 2), CH('v', 2), CH('b', 2), CH('n', 2), CH('m', 2), CH(',', 2), CH('.', 2)};
static const vk_key_t E_LET_R3[] = {LAY(VKL_SYM, 6, "123"), CHL(' ', 8, "SPACE"), KIND(VK_K_OK, 4, "OK"), KIND(VK_K_CANCEL, 2, "X")};
static const vk_key_t E_NUM_R0[] = {CH('1', 2), CH('2', 2), CH('3', 2), CH('4', 2), CH('5', 2), CH('6', 2), CH('7', 2), CH('8', 2), CH('9', 2), CH('0', 2)};
static const vk_key_t E_NUM_R1[] = {CH('!', 2), CH('@', 2), CH('#', 2), CH('$', 2), CH('%', 2), CH('^', 2), CH('&', 2), CH('*', 2), CH('(', 2), KIND(VK_K_BACK, 2, "<-")};
static const vk_key_t E_NUM_R2[] = {CH(')', 2), CH('-', 2), CH('_', 2), CH('=', 2), CH('+', 2), CH('[', 2), CH(']', 2), CH('{', 2), CH('}', 2), CH(';', 2)};
static const vk_key_t E_NUM_R3[] = {LAY(VKL_SYM2, 6, "!@#"), CHL(' ', 8, "SPACE"), KIND(VK_K_OK, 4, "OK"), KIND(VK_K_CANCEL, 2, "X")};
static const vk_key_t E_SYM_R0[] = {CH('~', 2), CH('`', 2), CH('|', 2), CH('\\', 2), CH('/', 2), CH('<', 2), CH('>', 2), CH('?', 2), CH('\'', 2), CH('"', 2)};
static const vk_key_t E_SYM_R1[] = {CH(':', 2), CH(';', 2), CH(',', 2), CH('.', 2), CH('!', 2), CH('@', 2), CH('#', 2), CH('$', 2), CH('%', 2), KIND(VK_K_BACK, 2, "<-")};
static const vk_key_t E_SYM_R2[] = {CH('&', 2), CH('*', 2), CH('(', 2), CH(')', 2), CH('+', 2), CH('=', 2), CH('^', 2), CH('-', 2), CH('_', 2), CH('~', 2)};
static const vk_key_t E_SYM_R3[] = {LAY(VKL_LETTERS, 6, "abc"), CHL(' ', 8, "SPACE"), KIND(VK_K_OK, 4, "OK"), KIND(VK_K_CANCEL, 2, "X")};

static const vk_row_t ENTRY_BASE[3][3] = {
    {ROW(E_LET_R0), ROW(E_LET_R1), ROW(E_LET_R2)},
    {ROW(E_NUM_R0), ROW(E_NUM_R1), ROW(E_NUM_R2)},
    {ROW(E_SYM_R0), ROW(E_SYM_R1), ROW(E_SYM_R2)},
};
static const vk_row_t ENTRY_LAST[3] = {
    ROW(E_LET_R3),
    ROW(E_NUM_R3),
    ROW(E_SYM_R3),
};

int vk_build(const vk_ui_t *u, vk_cell_t *out, int cap) {
    int layer = u->layer < 4 ? u->layer : VKL_LETTERS;
    if (u->entry && layer == VKL_CTL) layer = VKL_LETTERS;
    int variant = u->entry ? (u->mask ? 1 : 2) : 0;
    vk_row_t rows[VK_ROWS];
    if (u->entry) {
        for (int r = 0; r < 3; r++) rows[r] = ENTRY_BASE[layer][r];
        rows[3] = ENTRY_LAST[layer];
    } else {
        for (int r = 0; r < 3; r++) rows[r] = BASE[layer][r];
        rows[3] = LAST[layer][variant];
    }
    int n = 0;
    for (int r = 0; r < VK_ROWS; r++) {
        int x = 0;
        for (uint8_t i = 0; i < rows[r].n; i++) {
            if (n >= cap) return -1;
            const vk_key_t *k = &rows[r].k[i];
            out[n].row = (uint8_t)r;
            out[n].x = (uint8_t)x;
            out[n].w = k->w;
            out[n].key = k;
            n++;
            x += k->w;
        }
    }
    return n;
}

static int center2(const vk_cell_t *c) { return 2 * c->x + c->w; }

int vk_pref2(const vk_cell_t *c, int focus) { return center2(&c[focus]); }

int vk_focus_home(const vk_cell_t *c, int n, bool entry) {
    if (n <= 0) return 0;
    if (entry) { /* the reference's: the middle of the second row, the key covering x = 8 half-units */
        for (int i = 0; i < n; i++)
            if (c[i].row == 1 && c[i].x <= 8 && c[i].x + c[i].w > 8) return i;
        return 0;
    }
    /* The live keyboard: the bottom row, from its left end, its first layer key (abc, 123, #+=), so a symbol or a digit is one
     * Enter away and the same key again goes on to the next layer. Without a layer key there, the row's first key. */
    int last = c[n - 1].row, first = n - 1;
    while (first > 0 && c[first - 1].row == last) first--;
    for (int i = first; i < n; i++)
        if (c[i].key && c[i].key->kind == VK_K_LAYER) return i;
    return first;
}

int vk_move(const vk_cell_t *c, int n, int focus, vk_dir_t d, int *pref2) {
    if (n <= 0 || focus < 0 || focus >= n) return 0;
    int row = c[focus].row, s = focus, e = focus;
    while (s > 0 && c[s - 1].row == row) s--;
    while (e + 1 < n && c[e + 1].row == row) e++;
    int cnt = e - s + 1, pos = focus - s;
    switch (d) {
    case VK_DIR_LEFT: pos = (pos + cnt - 1) % cnt; *pref2 = center2(&c[s + pos]); return s + pos;
    case VK_DIR_RIGHT: pos = (pos + 1) % cnt; *pref2 = center2(&c[s + pos]); return s + pos;
    case VK_DIR_HOME: *pref2 = center2(&c[s]); return s;
    case VK_DIR_END: *pref2 = center2(&c[e]); return e;
    default: break;
    }
    int nr = row;
    if (d == VK_DIR_UP) nr = (row + VK_ROWS - 1) % VK_ROWS;
    else if (d == VK_DIR_DOWN) nr = (row + 1) % VK_ROWS;
    else if (d == VK_DIR_PGUP) nr = 0;
    else if (d == VK_DIR_PGDN) nr = VK_ROWS - 1;
    int best = -1, bestd = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].row != nr) continue;
        int dd = center2(&c[i]) - *pref2;
        if (dd < 0) dd = -dd;
        if (best < 0 || dd < bestd) {
            best = i;
            bestd = dd;
        }
    }
    return best < 0 ? focus : best;
}

void vk_shift_tap(vk_ui_t *u, uint32_t now_ms) {
    switch (u->shift) {
    case VK_SHIFT_OFF: u->shift = VK_SHIFT_ONE; break;
    case VK_SHIFT_ONE: u->shift = (uint32_t)(now_ms - u->shift_tap_ms) < 400u ? VK_SHIFT_CAPS : VK_SHIFT_OFF; break;
    default: u->shift = VK_SHIFT_OFF; break;
    }
    u->shift_tap_ms = now_ms;
}

void vk_cycle_layer(vk_ui_t *u) {
    int l = (u->layer + 1) % 4;
    if (u->entry && l == VKL_CTL) l = VKL_LETTERS;
    u->layer = (uint8_t)l;
}

uint32_t vk_upper(uint32_t cp) { return (cp >= 'a' && cp <= 'z') ? cp - 32u : cp; }

int vk_is_upper(const vk_ui_t *u, uint8_t phys_mods) { return u->shift != VK_SHIFT_OFF || (phys_mods & PAL_MOD_SHIFT) != 0; }

int vk_key_event(const vk_key_t *k, int upper, pal_key_t *out) {
    out->repeat = false;
    out->mods = 0;
    out->cp = 0;
    out->sym = PAL_KEY_NONE;
    switch (k->kind) {
    case VK_K_CHAR:
        out->sym = PAL_KEY_CHAR;
        out->cp = upper ? vk_upper(k->cp) : k->cp;
        if (upper) out->mods = PAL_MOD_SHIFT;
        return 1;
    case VK_K_CHORD:
        out->sym = PAL_KEY_CHAR;
        out->cp = k->cp;
        out->mods = PAL_MOD_CTRL;
        return 1;
    case VK_K_BACK: out->sym = PAL_KEY_BACKSPACE; return 1;
    case VK_K_ENTER:
    case VK_K_OK: out->sym = PAL_KEY_ENTER; return 1;
    case VK_K_SPECIAL: out->sym = k->sym; return 1;
    default: return 0;
    }
}

size_t vk_utf8_put(char *out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t vk_text_insert(char *buf, size_t len, size_t cap, uint32_t cp) {
    char tmp[4];
    size_t n = vk_utf8_put(tmp, cp);
    if (len + n + 1 > cap) return len;
    memcpy(buf + len, tmp, n);
    len += n;
    buf[len] = 0;
    return len;
}

size_t vk_text_backspace(char *buf, size_t len) {
    if (len == 0) return 0;
    size_t p = len - 1;
    while (p > 0 && ((uint8_t)buf[p] & 0xC0u) == 0x80u) p--;
    buf[p] = 0;
    return p;
}

size_t vk_cp_count(const char *s, size_t len) { return vk_cp_index(s, len, len); }

size_t vk_cp_index(const char *s, size_t len, size_t off) {
    size_t n = 0;
    if (off > len) off = len;
    for (size_t i = 0; i < off; i++)
        if (((uint8_t)s[i] & 0xC0u) != 0x80u) n++;
    return n;
}

size_t vk_scroll_for(size_t scroll, size_t caret, size_t vis) {
    if (vis == 0 || caret < scroll) return caret;
    if (caret >= scroll + vis) return caret - vis + 1;
    return scroll;
}
