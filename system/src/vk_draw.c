/* Drawing of the on-screen keyboard (see vk.c for the state), in the reference look): the prompt on top, a rule under it, the text field with a '_' caret, a rule above the keys,
 * and the keys on a 16 px grid (8 px per half-unit) with 13 px rows and no gaps. The focused key is inverted on a rounded
 * box. Drawn through kui on the logical 1-bit canvas, so the pixels are the reference's. The whole screen is painted each time (the
 * keyboard is small); the dirty marks are still cleared so the controller's bookkeeping is unchanged. */
#include <string.h>

#include "nema/types.h"
#include "pal/kui.h"
#include "pal/shell.h"
#include "pal/vk_layout.h"
#include "vk_int.h"

#define HU_PX 8         /* one half-unit of a key row, in logical pixels: 20 half-units = 160 */
#define KEY_H 13        /* The reference STEPY (KH_MAX) */
#define PROMPT_Y 14     /* The reference TOP = SEP1_Y + 2 */
#define RULE_Y 24       /* TOP + 10 */
#define FIELD_Y 27      /* TOP + 13 */
#define FIELD_X 2
#define FIELD_COLS 26   /* (160 - 4) / CHAR_W */

static kui_canvas_t VK_KC NEMA_EXT_BSS;

/* The field line as the reference shows it: the text (asterisks when masked), the '_' caret at the cursor. The window follows the
 * caret, so a long text shows its end, as the reference tail does. */
static void draw_field(kui_canvas_t *c) {
    char cells[VK_TEXT_MAX + 4];
    size_t n = vk_cp_count(vk_S.text, vk_S.len);
    size_t caret = vk_cp_index(vk_S.text, vk_S.len, vk_S.cursor);
    bool masked = vk_S.ui.mask && !vk_S.ui.show;
    size_t k = 0, off = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == caret) cells[k++] = '_';
        if (i >= n) break;
        unsigned char ch = (unsigned char)vk_S.text[off];
        cells[k++] = masked ? '*' : (ch < 0x80 ? (char)ch : '?');
        off++;
        while (off < vk_S.len && ((unsigned char)vk_S.text[off] & 0xC0u) == 0x80u) off++;
    }
    cells[k] = 0;
    size_t first = caret >= FIELD_COLS ? caret - FIELD_COLS + 1 : 0;
    char shown[FIELD_COLS + 1];
    size_t m = 0;
    for (size_t i = first; i < k && m < FIELD_COLS; i++) shown[m++] = cells[i];
    shown[m] = 0;
    kui_text(c, &FONT_REG8, FIELD_X, FIELD_Y, shown, true);
    vk_S.cursor_cp = caret;
    vk_S.scroll = first;
}

/* The label of a key cell: its face in the current state. */
static const char *key_label(const vk_key_t *k, uint8_t phys, char *buf, size_t cap) {
    switch (k->kind) {
    case VK_K_CHAR:
        if (k->label) return k->label;
        buf[0] = (char)(vk_is_upper(&vk_S.ui, phys) ? vk_upper(k->cp) : k->cp);
        buf[1] = 0;
        (void)cap;
        return buf;
    case VK_K_SHIFT: return vk_S.ui.shift != VK_SHIFT_OFF ? "^" : "v"; /* The reference caps cap: ^ upper, v lower */
    case VK_K_BACK: return "<-";
    case VK_K_OK: return "OK";
    case VK_K_CANCEL: return "X";
    case VK_K_SHOW: return vk_S.ui.show ? "hide" : "show";
    case VK_K_ENTER: return k->label ? k->label : "ENTER";
    case VK_K_SPECIAL:
        if (k->label) return k->label;
        if (k->sym == PAL_KEY_LEFT) return "<";
        if (k->sym == PAL_KEY_RIGHT) return ">";
        if (k->sym == PAL_KEY_UP) return "^";
        return "v";
    default: return k->label ? k->label : "";
    }
}

/* Keys hang from the bottom of the screen: four rows of 13 px, the first at h - 52 (the reference KY). */
static void draw_key(kui_canvas_t *c, int idx, uint8_t phys) {
    const vk_cell_t *cell = &vk_S.cells[idx];
    char buf[4];
    const char *label = key_label(cell->key, phys, buf, sizeof(buf));
    int x = cell->x * HU_PX, w = cell->w * HU_PX;
    int y = (c->h - VK_ROWS * KEY_H) + cell->row * KEY_H;
    bool sel = idx == vk_S.focus || idx == vk_S.flash;
    kui_key(c, x, y, w, KEY_H, label, sel);
}

void vk_draw(pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)full;
    kui_canvas_init(&VK_KC, c->w, c->h, kui_scale_q());
    kui_canvas_t *k = &VK_KC;
    kui_clear(k, false);

    const char *prompt = vk_S.title;
    kui_text(k, &FONT_REG8, FIELD_X, PROMPT_Y, prompt ? prompt : "", true);
    kui_fill(k, 0, RULE_Y, k->w, 1, true);
    draw_field(k);
    int key_top = k->h - VK_ROWS * KEY_H;
    kui_fill(k, 0, key_top - 1, k->w, 1, true);

    uint8_t phys = vk_S.phys;
    for (int i = 0; i < vk_S.ncells; i++) draw_key(k, i, phys);

    kui_present(c, k, a);
    vk_S.need_full = false;
    vk_S.dirty_text = false;
    vk_S.dirty_caret = false;
    memset(vk_S.dirty_keys, 0, sizeof(vk_S.dirty_keys));
}

/* The keyboard as data (pal/shell.h): a session that draws the shell services lays it out itself. */
static char s_entry[VK_TEXT_MAX + 1] NEMA_EXT_BSS;
bool vk_describe(pal_view_t *v, pal_shell_view_t *o) {
    (void)v;
    bool masked = vk_S.ui.mask && !vk_S.ui.show;
    size_t n = vk_S.len < VK_TEXT_MAX ? vk_S.len : VK_TEXT_MAX;
    for (size_t i = 0; i < n; i++) s_entry[i] = masked ? '*' : vk_S.text[i];
    s_entry[n] = 0;
    o->kind = PAL_SHELL_KEYBOARD;
    o->title = vk_S.title;
    o->text = s_entry;
    o->cursor = (int)vk_S.cursor;
    o->rows = VK_ROWS;
    o->nkeys = vk_S.ncells < PAL_SHELL_KEYS ? vk_S.ncells : PAL_SHELL_KEYS;
    o->focus = vk_S.flash >= 0 ? vk_S.flash : vk_S.focus;
    for (int i = 0; i < o->nkeys; i++) {
        const vk_cell_t *c = &vk_S.cells[i];
        char buf[4];
        pal_shell_key_t *k = &o->keys[i];
        k->row = c->row, k->x = c->x, k->w = c->w;
        nema_strlcpy(k->label, key_label(c->key, vk_S.phys, buf, sizeof(buf)), sizeof(k->label));
        k->accent = c->key->kind != VK_K_CHAR && c->key->kind != VK_K_CHORD;
    }
    return true;
}
