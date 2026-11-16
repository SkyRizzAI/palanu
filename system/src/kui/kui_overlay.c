/* kui overlays: the reference modal dialog (the dimmed backdrop, the box and its content), the Toast band, the Popup and the list
 * page of a list-style modal (with the ListSection and ListItemRow widgets).
 *
 * Layout follows the reference flex rules for these trees: a column with its children stacked and centred across, the gap between
 * them, and each button a bordered box with 4 px of padding around its label. Sizes are logical (2 px per pixel).
 * The reference Text does not wrap, so the body is one line and a '\n' shows as a blank glyph advance, as the reference shows it. PalanuOS
 * callers pass wrap = true: the body then breaks at spaces and the box grows to hold the lines (never below the reference 80 px). */
#include <string.h>

#include "nema/types.h"
#include "pal/kui.h"
#include "pal/theme.h"

#define DLG_PAD 8      /* The reference space.md */
#define DLG_GAP 4      /* The reference space.sm: between children, and inside a button */
#define BTN_GAP 4      /* the gap between buttons in their row */
#define LIST_INSET 2   /* ListContainer padding */
#define LIST_BAR_X 157 /* the scrollbar's x (logical): container right edge minus its 3 px bar inset */
#define WRAP_LINES 6   /* body lines a wrapped dialog holds */
#define WRAP_CHARS 96  /* longest line kept, NUL included */

static kui_canvas_t S NEMA_EXT_BSS; /* the overlay canvas; overlays are drawn on the desktop task only */

void kui_backdrop(pal_canvas_t *dst, pal_rect_t r, int top, int scale_q) {
    int x0 = r.x < dst->clip_x0 ? dst->clip_x0 : r.x;
    int y0 = r.y < dst->clip_y0 ? dst->clip_y0 : r.y;
    int x1 = r.x + r.w > dst->clip_x1 ? dst->clip_x1 : r.x + r.w;
    int y1 = r.y + r.h > dst->clip_y1 ? dst->clip_y1 : r.y + r.h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) {
        int ly = y * KUI_Q / scale_q;
        if (ly < top) continue;
        uint8_t *row = dst->px + y * dst->stride;
        for (int x = x0; x < x1; x++)
            if (((x * KUI_Q / scale_q + ly) & 1) == 0) row[x] = PAL_C_INK;
    }
    pal_mark(dst, x0, y0, x1 - x0, y1 - y0);
}

/* Breaks one line (no '\n') into lines of at most w pixels, at spaces where it can. Returns the number of lines kept. */
static int wrap_line(const char *s, int w, char out[][WRAP_CHARS], int n, int max) {
    size_t len = strlen(s), i = 0;
    while (i < len && n < max) {
        size_t j = i, sp = 0;
        bool has_sp = false;
        char tmp[WRAP_CHARS];
        while (j < len && j - i < WRAP_CHARS - 1) {
            size_t k = j + 1;
            memcpy(tmp, s + i, k - i);
            tmp[k - i] = 0;
            if (kui_text_w(&FONT_REG8, tmp) > w) break;
            if (s[j] == ' ') { sp = j; has_sp = true; }
            j++;
        }
        size_t end = j >= len ? len : (has_sp && sp > i ? sp : (j > i ? j : i + 1));
        size_t m = end - i;
        if (m >= WRAP_CHARS) m = WRAP_CHARS - 1;
        memcpy(out[n], s + i, m);
        out[n][m] = 0;
        n++;
        i = end;
        while (i < len && s[i] == ' ') i++;
    }
    return n;
}

/* Splits text at its '\n' and wraps each piece to w pixels. */
static int wrap_text(const char *text, int w, char out[][WRAP_CHARS], int max) {
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        char seg[WRAP_CHARS * 2];
        size_t m = (size_t)(e - p);
        if (m >= sizeof(seg)) m = sizeof(seg) - 1;
        memcpy(seg, p, m);
        seg[m] = 0;
        if (m == 0) {
            if (n < max) out[n++][0] = 0; /* an empty line keeps its place */
        } else {
            n = wrap_line(seg, w, out, n, max);
        }
        p = *e ? e + 1 : e;
    }
    return n;
}

static pal_rect_t dialog(pal_canvas_t *dst, pal_rect_t area, const char *title, const char *body,
                         const char *const *buttons, int nbuttons, int selected, bool wrap) {
    kui_canvas_init(&S, dst->w, dst->h, kui_scale_q());
    kui_backdrop(dst, area, kui_lg(&S, area.y), S.sq); /* The reference dithers from the status bar's bottom row down */

    const int W = S.w, H = S.h;
    const int inner_w = (W < KUI_DIALOG_W ? W : KUI_DIALOG_W) - 2 * DLG_PAD; /* the inside width of the box */
    char lines[WRAP_LINES][WRAP_CHARS];
    int nl = 0;
    const int lineH = FONT_REG8.charH + 1;
    if (wrap && body && body[0]) nl = wrap_text(body, inner_w, lines, WRAP_LINES);

    /* the content's height, as the reference column measures it */
    const int btn_h = FONT_REG8.charH + 2 * DLG_GAP;
    int content = 0, kids = 0;
    if (title && title[0]) { content += FONT_BOLD10.charH; kids++; }
    if (wrap ? nl > 0 : (body && body[0])) {
        if (kids) content += DLG_GAP;
        content += wrap ? nl * lineH - 1 : FONT_REG8.charH;
        kids++;
    }
    if (buttons && nbuttons > 0) {
        if (kids) content += DLG_GAP;
        content += btn_h;
    }
    int mh = H < KUI_DIALOG_H ? H : KUI_DIALOG_H;
    if (wrap && content + 2 * DLG_PAD > mh) mh = content + 2 * DLG_PAD < H ? content + 2 * DLG_PAD : H;
    const int mw = W < KUI_DIALOG_W ? W : KUI_DIALOG_W;
    const int mx = (W - mw) / 2, my = (H - mh) / 2;
    kui_round(&S, mx, my, mw, mh, 1, true); /* the box: paper inside (the canvas starts paper), outline with cut corners */

    const int ix = mx + DLG_PAD, iw = mw - 2 * DLG_PAD;
    int y = my + DLG_PAD;
    int placed = 0; /* children so far: a gap goes before each one after the first */

    if (title && title[0]) {
        int off = (iw - kui_text_w(&FONT_BOLD10, title)) / 2;
        kui_text(&S, &FONT_BOLD10, ix + (off > 0 ? off : 0), y, title, true);
        y += FONT_BOLD10.charH;
        placed++;
    }
    if (wrap) {
        if (nl > 0) {
            if (placed) y += DLG_GAP;
            for (int i = 0; i < nl; i++, y += lineH) {
                int off = (iw - kui_text_w(&FONT_REG8, lines[i])) / 2;
                kui_text(&S, &FONT_REG8, ix + (off > 0 ? off : 0), y, lines[i], true);
            }
            y -= 1; /* the body is nl lines of lineH less the last line's spare pixel, as in the height above */
            placed++;
        }
    } else if (body && body[0]) {
        if (placed) y += DLG_GAP;
        int off = (iw - kui_text_w(&FONT_REG8, body)) / 2;
        kui_text(&S, &FONT_REG8, ix + (off > 0 ? off : 0), y, body, true);
        y += FONT_REG8.charH;
        placed++;
    }
    if (buttons && nbuttons > 0) {
        if (placed) y += DLG_GAP;
        kui_buttons(&S, ix, y, iw, buttons, nbuttons, selected);
    }

    pal_rect_t box = {kui_ph(S.sq, mx), kui_ph(S.sq, my), kui_ph(S.sq, mx + mw) - kui_ph(S.sq, mx), kui_ph(S.sq, my + mh) - kui_ph(S.sq, my)};
    kui_present(dst, &S, box);
    return box;
}

int kui_buttons(kui_canvas_t *c, int x, int y, int w, const char *const *labels, int n, int sel) {
    const int bh = FONT_REG8.charH + 2 * DLG_GAP; /* label plus 4 px above and below */
    int row = BTN_GAP * (n - 1);
    for (int i = 0; i < n; i++) row += kui_text_w(&FONT_REG8, labels[i] ? labels[i] : "") + 2 * DLG_GAP;
    int off = (w - row) / 2, bx = x + (off > 0 ? off : 0);
    for (int i = 0; i < n; i++) {
        const char *lab = labels[i] ? labels[i] : "";
        int bw = kui_text_w(&FONT_REG8, lab) + 2 * DLG_GAP;
        kui_round(c, bx, y, bw, bh, 1, true);
        kui_text(c, &FONT_REG8, bx + DLG_GAP, y + DLG_GAP, lab, true);
        if (i == sel) kui_invert(c, bx + 1, y + 1, bw - 2, bh - 2); /* the focus fills the inside, the outline stays */
        bx += bw + BTN_GAP;
    }
    return bh;
}

pal_rect_t kui_dialog(pal_canvas_t *dst, pal_rect_t area, const char *title, const char *body,
                      const char *const *buttons, int nbuttons, int selected, bool wrap) {
    return dialog(dst, area, title, body, buttons, nbuttons, selected, wrap);
}

void kui_toast_band(pal_canvas_t *dst, pal_rect_t box, const char *text) {
    kui_canvas_init(&S, dst->w, dst->h, kui_scale_q());
    const int top = kui_lg(&S, box.y);
    /* The band is ink with radius-1 corners, as the reference's. The reference draws its caption in ink, which vanishes on the band; PalanuOS
     * deliberately draws it in paper instead so the message is readable. The caption sits at the Row's 4 px padding. */
    kui_fill_round(&S, 0, top, S.w, KUI_TOAST_LH, 1, true);
    if (text && text[0]) kui_text(&S, &FONT_REG8, 4, top + 4, text, false);
    kui_present(dst, &S, box);
}

void kui_popup(kui_canvas_t *c, int y, const char *text) {
    if (!text || !text[0]) return;
    const int inner = c->w - 2 * DLG_PAD;
    int off = (inner - kui_text_w(&FONT_REG8, text)) / 2;
    kui_text(c, &FONT_REG8, DLG_PAD + (off > 0 ? off : 0), y, text, true);
}

int kui_list_page(kui_canvas_t *c, int x, int y, int w, int h, const char *title, const char *const *labels, int n, int sel,
                  int first) {
    int top = y + kui_section(c, x, y, &FONT_BOLD8, title ? title : "");
    const int bottom = y + h;
    const int list_y = top + LIST_INSET;
    const int list_h = bottom - list_y;
    const int content = n > 0 ? n * KUI_LIST_PITCH - 2 : 0; /* every row and the gaps between them */
    const bool bar = content > list_h;
    const int rw = w - 2 * LIST_INSET - (bar ? 3 : 0);
    if (first < 0) first = 0;
    int ry = list_y;
    kui_clip(c, 0, list_y, c->w, list_h); /* The reference container clips the row cut by its bottom edge */
    for (int i = first; i < n && ry < bottom; i++, ry += KUI_LIST_PITCH) {
        kui_list_item_t it = {0};
        it.label = labels[i] ? labels[i] : "";
        it.icon = -1;
        it.selected = i == sel;
        kui_list_item(c, x + LIST_INSET, ry, rw, &it);
    }
    kui_clip_reset(c);
    if (bar) kui_scrollbar(c, LIST_BAR_X, list_y, list_h, first * KUI_LIST_PITCH, list_h, content);
    return ry;
}
