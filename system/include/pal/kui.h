/* PalanuOS kui: the reference drawing layer in C (translated from the reference sources and include/nema/ui).
 *
 * The reference draws a LOGICAL 1-bit canvas (160x120 at scale 2) and its driver expands every logical pixel to a scale x scale
 * block of the 320x240 panel, with ink (on) and paper (off) only. kui keeps the same model: kui_canvas_t is a packed
 * 1-bit logical canvas with the reference clip and primitive semantics, and kui_present() expands a physical rectangle of it
 * into a PalanuOS pal_canvas_t using the two palette slots PAL_C_INK (on) and PAL_C_PAPER (off).
 *
 * Fonts are the reference BitmapFont data unchanged (column-major glyphs, see system/src/kui/font_*.c). */
#ifndef PAL_KUI_H
#define PAL_KUI_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/gfx.h"

#define KUI_LW 160 /* the logical size the screens were designed for: the reference 160x120 at scale 2 on a 320x240 panel */
#define KUI_LH 120
#define KUI_MAX_W 320 /* the largest logical canvas: scale 1 on a 320x240 panel */
#define KUI_MAX_H 240

/* UI scale, in quarters of a physical pixel per logical pixel: 4 is 1x (320x240 logical on hhv2), 8 is 2x (160x120, the
 * design size), 10 is 2.5x, the largest. Quarter steps between are allowed: some logical pixels are then one physical pixel wider than
 * their neighbours (nearest neighbour), the price of a size between two whole scales. Every screen uses the same scale,
 * kui_scale_q(); the board gives the default and Settings > Display > UI Scale changes it (ui.scale). */
#define KUI_Q 4
#define KUI_SCALE_MIN_Q 4
#define KUI_SCALE_MAX_Q 10 /* 2.5x: 128x96 on hhv2. Smaller canvases break the 160x120 layouts (launcher, control, home) */
int kui_scale_q(void);
void kui_set_scale_q(int q); /* clamped to KUI_SCALE_MIN_Q..KUI_SCALE_MAX_Q; the caller repaints */
/* Physical height of a band of n logical rows at the top of the screen (the status bar, the toast under it), at the
 * current scale: the rows whose logical row is below n. */
int kui_top_px(int n);
/* Physical height of a band of n logical rows at the bottom of a screen phys_h tall (the legend bar). */
int kui_bottom_px(int phys_h, int n);

/* Same field order and names as the reference BitmapFont, so the ported font tables initialise unchanged. */
typedef struct {
    const uint8_t *data;       /* column-major glyph bits; bit 0 of byte 0 = top row */
    uint8_t charW;             /* monospace width, or proportional advance fallback */
    uint8_t charH;             /* pixel height of a glyph cell */
    uint8_t firstChar;         /* first code point in data (usually 0x20) */
    uint8_t numChars;
    uint8_t spacing;           /* extra pixels between glyphs */
    const uint8_t *widths;     /* per-glyph widths, or NULL for monospace */
    const uint16_t *offsets;   /* per-glyph byte offsets, or NULL for monospace */
    uint8_t bytesPerCol;       /* 1 for up to 8 rows, 2 for up to 16; 0 means 1 */
} kui_font_t;

extern const kui_font_t FONT_5X8, FONT_6X8, FONT_TINY, FONT_PRIMARY, FONT_MONO;
extern const kui_font_t FONT_REG8, FONT_BOLD8, FONT_REG10, FONT_BOLD10, FONT_REG12, FONT_BOLD12;

/* Logical 1-bit canvas. Bit set = ink (on). Rows are KUI_MAX_W bits; bit i is MSB-first in its byte. 9.6 KB: a static
 * canvas is written NEMA_EXT_BSS (docs/adr/0002). */
typedef struct {
    uint8_t bits[KUI_MAX_W * KUI_MAX_H / 8];
    int sq;                                /* the scale in quarters (KUI_Q) */
    int scale;                             /* the whole scale nearest to it, for code that wants an integer */
    int w, h;                              /* logical size */
    int clip_x0, clip_y0, clip_x1, clip_y1; /* logical clip, exclusive upper bounds */
} kui_canvas_t;

/* ---- canvas (kui_canvas.c) ---- */
void kui_canvas_init(kui_canvas_t *c, int phys_w, int phys_h, int scale_q); /* scale_q: normally kui_scale_q() */
/* Physical to logical (rounded down): the logical row or column a physical one shows. */
static inline int kui_lg(const kui_canvas_t *c, int phys) { return phys * KUI_Q / c->sq; }
/* The first physical row (or column) that shows logical row l: the inverse of kui_lg, rounded up. */
static inline int kui_ph(int scale_q, int l) { return (l * scale_q + KUI_Q - 1) / KUI_Q; }
void kui_clip(kui_canvas_t *c, int x, int y, int w, int h);
void kui_clip_reset(kui_canvas_t *c);
void kui_clear(kui_canvas_t *c, bool on);
void kui_pixel(kui_canvas_t *c, int x, int y, bool on);
bool kui_get(const kui_canvas_t *c, int x, int y);
void kui_fill(kui_canvas_t *c, int x, int y, int w, int h, bool on);
void kui_invert(kui_canvas_t *c, int x, int y, int w, int h);
void kui_frame(kui_canvas_t *c, int x, int y, int w, int h, bool on); /* outline */
void kui_line(kui_canvas_t *c, int x0, int y0, int x1, int y1, bool on);
/* Rounded boxes: r is the corner inset in pixels (1 clips the corners, 2+ chamfers with a 45 degree diagonal). */
void kui_fill_round(kui_canvas_t *c, int x, int y, int w, int h, int r, bool on);
void kui_round(kui_canvas_t *c, int x, int y, int w, int h, int r, bool on);
/* XBM-style bitmap: rows padded to bytes, MSB first, 1 = on. */
void kui_bitmap(kui_canvas_t *c, int x, int y, int w, int h, const uint8_t *bits, bool on);

/* Expand the physical rectangle r of the logical canvas into dst: ink where the bit is set, else paper. */
void kui_present(pal_canvas_t *dst, const kui_canvas_t *c, pal_rect_t r);

/* ---- text (kui_text.c) ---- */
int kui_text_w(const kui_font_t *f, const char *s);            /* The reference textWidth */
void kui_text(kui_canvas_t *c, const kui_font_t *f, int x, int y, const char *s, bool on); /* The reference drawText */
void kui_char(kui_canvas_t *c, const kui_font_t *f, int x, int y, char ch, bool on);      /* The reference drawChar */
/* The same glyphs at an integer scale: each lit logical pixel becomes a scale x scale block (the home clock). */
void kui_text_scaled(kui_canvas_t *c, const kui_font_t *f, int x, int y, const char *s, int scale, bool on);
int kui_text_w_scaled(const kui_font_t *f, const char *s, int scale);

/* ---- theme (kui_style.c): the two colours of the active palette, dark mode swaps them ---- */
typedef struct {
    const char *name; /* "palanu", "mono", "orange" */
    uint32_t ink;     /* 0xRRGGBB on colour */
    uint32_t paper;   /* 0xRRGGBB off colour */
} kui_palette_t;
const kui_palette_t *kui_palette_named(const char *name); /* unknown names give palanu */
const kui_palette_t *kui_palette_dark(const kui_palette_t *p); /* same colours, ink and paper swapped */

/* ---- widgets (kui_status.c) ---- */
typedef struct {
    int hour, minute; /* clock, 3x5 glyphs */
    int battery;      /* 0..100, or -1 when unknown (no battery drawn) */
    bool wifi;        /* Wi-Fi signal bars */
} kui_status_t;

#define KUI_STATUS_H 11 /* The reference STATUS_H + 2 logical pixels: 22 physical */
#define KUI_STATUS_ITEMS_X 22 /* where the status items start, logical: the clock ends at 19 (kui_status.c), a gap of 3 */
void kui_status_draw(kui_canvas_t *c, const kui_status_t *st);

/* ---- list rows (kui_list.c): the reference ListItemRow / ListInputRow / Switch / scrollbar, logical coordinates ---- */
void kui_row_frame(kui_canvas_t *c, int x, int y, int w, int h, bool selected); /* inverts the selection box */
void kui_row_label(kui_canvas_t *c, int x, int y, int h, int avail, const char *label);
void kui_row_chevron(kui_canvas_t *c, int right, int y, int h, bool next);
void kui_switch(kui_canvas_t *c, int x, int y, bool on);
void kui_row_switch(kui_canvas_t *c, int x, int y, int h, int w, bool on);
void kui_row_choice(kui_canvas_t *c, int x, int y, int h, int w, const char *label, const char *value, bool can_prev,
                    bool can_next);
void kui_scrollbar(kui_canvas_t *c, int x, int y, int size, int offset, int viewport, int content);

/* ---- keyboard (kui_keyboard.c): one key of the reference VirtualKeyboard grid, logical coordinates ---- */
void kui_key(kui_canvas_t *c, int x, int y, int w, int h, const char *label, bool sel);
/* ---- files and logs (kui_files.c): the reference ListSection, the file icons (icon_pack file.*) and ListItemRow with an icon ---- */
#define KUI_ICON_FOLDER 0
#define KUI_ICON_FILE 1
#define KUI_ICON_GENERIC 2
#define KUI_LIST_ROW_H 15 /* The reference listRowH: REG8 cell (11) + 4 */
#define KUI_LIST_PITCH 17 /* a row and the 2 px gap after it */
typedef struct {
    const char *label;
    int icon;          /* KUI_ICON_* or -1 for no icon */
    const char *value; /* REG8 text before the chevron, or NULL */
    bool chevron;      /* a '>' at the right edge */
    bool selected;     /* the inverted focus box */
} kui_list_item_t;
void kui_icon8(kui_canvas_t *c, int x, int y, int icon, bool on);
/* The reference ListSection: 4 px of space, the title in font f at x+5, 1 px of space. Returns its height. */
int kui_section(kui_canvas_t *c, int x, int y, const kui_font_t *f, const char *title);
/* One ListItemRow of width w at (x, y), KUI_LIST_ROW_H tall. */
void kui_list_item(kui_canvas_t *c, int x, int y, int w, const kui_list_item_t *it);
/* The reference ListInputRow (label, < value >) of width w at (x, y); the value column takes the flex remainder. */
void kui_list_choice(kui_canvas_t *c, int x, int y, int w, const char *label, const char *value, bool can_prev, bool can_next);
/* ---- desktop (kui_desktop.c, area: wallpaper). The reference LiveWallpaperDesktop::draw / blitFit, logical coordinates. Fit and
 * anchor use the reference enum order (desktop_theme.h). ---- */
#define KUI_FIT_CENTER 0
#define KUI_FIT_STRETCH 1
#define KUI_FIT_CROP 2
#define KUI_FIT_FIT 3
/* The reference blitFit: scales the 1-bit frame bits (sw x sh, MSB first, row by row; bit set = ink) into the rectangle t, both on and
 * off pixels, and never outside it. Float arithmetic as in the reference, so the truncated sample positions are the same. */
void kui_wall_fit(kui_canvas_t *c, const uint8_t *bits, int sw, int sh, int tx, int ty, int tw, int th, int fit, int anchor);

/* ---- legend bar (kui_legend.c, pal/legend.h): one legend item, its key cap in ink with the key in paper and the label in ink.
 * Logical coordinates, FONT_6X8. The bar is KUI_LEGEND_H rows: 1 row of paper, the 9 row cap, 1 row of paper. ---- */
#define KUI_LEGEND_H 11
#define KUI_LEGEND_ENTER "\x01" /* as a key cap text: the Enter symbol, drawn as an icon instead of text */
int kui_legend_item_w(const char *key, const char *label); /* the width kui_legend_item draws */
int kui_legend_item(kui_canvas_t *c, int x, int y, const char *key, const char *label); /* returns the width drawn */

/* ---- splash (kui_splash.c, kui_splash_logo.c): the reference SplashScreen. The logo and a progress bar (0..1) centred as a block. ---- */
void kui_splash_draw(kui_canvas_t *c, float progress);
/* ---- launcher skins (kui_launcher.c; launcher area): the four launcher skins: strip, grid, compact, list.
 * Each skin draws inside the canvas clip (the area under the status bar): the top is clip_y0, the size is c->w x c->h,
 * as the reference skins read contentY() and the canvas size. ---- */
typedef struct {
    const char *label;
    const uint8_t *bits; /* 1-bit icon, rows padded to whole bytes, MSB first; set bits are ink */
    int w, h;            /* the icon's own size; drawn nearest-neighbour scaled, as the reference blitScaledMask */
    const uint8_t *small_bits; /* the 16 px variant of the icon for the small tiles of strip, or NULL */
    int small_w, small_h;
} kui_item_t;

/* The reference blitScaledMask: each set source pixel of an sw x sh icon paints `on` in the w x h box at (x, y). */
void kui_bitmap_scaled(kui_canvas_t *c, const uint8_t *bits, int sw, int sh, int x, int y, int w, int h, bool on);
/* The reference draw::scrollbar, horizontal: a dashed track on row y+1 and the 3 px thumb on row y. */
void kui_hscrollbar(kui_canvas_t *c, int x, int y, int size, int offset, int viewport, int content);
/* title: the device name drawn top-left by strip (NULL or empty reads PALANU). n <= 0 draws nothing (the reference returns early). */
void kui_skin_strip(kui_canvas_t *c, const char *title, const kui_item_t *it, int n, int cursor);
void kui_skin_grid(kui_canvas_t *c, const kui_item_t *it, int n, int cursor);
void kui_skin_compact(kui_canvas_t *c, const kui_item_t *it, int n, int cursor);
void kui_skin_list(kui_canvas_t *c, const kui_item_t *it, int n, int cursor);

/* ---- app list (kui_applist.c; applist area): the reference AppListScreen rows, ListItemRow in a VirtualList ---- */
typedef struct {
    const char *label;
    const uint8_t *bits; /* icon, drawn at 8 px (scaled when larger, at its own size when 8 or smaller); NULL for none */
    int w, h;
    bool chevron;       /* the row drills in: a '>' at the right */
} kui_row_t;

/* Rows of 12 px on a 14 px pitch from the area top, inset 2 px. first is the scroll (kept by the caller) and moves only as
 * far as needed to show cursor, as VirtualList does. */
void kui_applist(kui_canvas_t *c, const kui_row_t *rows, int n, int cursor, int *first);
/* ---- font packs (kui_fonts.c): the reference six text slots. Helvetica is the built-in set; Appearances > Font (ui.font) swaps
 * the slots as the reference FontRegistry::applyFontPack does. kui_slot returns the font now in a slot, read per call, so a change
 * applies at the next frame. kui_font_pack_set keeps the current pack when the name is unknown. */
typedef enum { KUI_F_REG8, KUI_F_BOLD8, KUI_F_REG10, KUI_F_BOLD10, KUI_F_REG12, KUI_F_BOLD12, KUI_F_COUNT } kui_slot_t;
const kui_font_t *kui_slot(kui_slot_t s);
bool kui_font_pack_set(const char *name); /* NULL or "" selects Helvetica; false when the name is unknown */
const char *kui_font_pack_name(void);     /* the pack in use */
int kui_font_pack_count(void);
const char *kui_font_pack_at(int i);      /* NULL when i is out of range */

/* ---- overlays (kui_overlay.c): the reference modal dialog over a dimmed screen, the toast band, the popup and the list page of the
 * picker. The rectangles are physical (PalanuOS canvas); the drawing is logical, 2 px per pixel. Desktop task only. ---- */
#define KUI_DIALOG_W 210 /* The reference ConfirmModal modalWidth: clamped to the canvas (160 logical) */
#define KUI_DIALOG_H 80  /* The reference ConfirmModal modalHeight */
#define KUI_TOAST_LH 19  /* The reference Toast: a full-width band of 19 logical rows (FONT_REG8 cell + 8) below the status bar */
/* The display-server dim overlay: the rows from top (logical) get a checker of ink, a pixel ink where (x + y) is even, painted over
 * whatever is on the physical canvas inside r. Repainting it is harmless (ink stays ink), so a partial redraw can run it. */
void kui_backdrop(pal_canvas_t *dst, pal_rect_t r, int top, int scale_q);
/* The reference Modal + Dialog + ConfirmModal: the backdrop over area (from its top), then the box centred on the canvas (paper,
 * one-pixel outline with the corners cut), title in Bold10, body in Reg8, and buttons (Reg8 text, 4 px inset, 4 px apart,
 * centred in a row). title and body may be NULL or empty. buttons[0] is the left one; selected is inverted. Returns the box.
 * wrap = false draws the reference's exact single line (a '\n' is a blank advance, a long body is cut at the edge). wrap = true breaks
 * the body at spaces and grows the box to fit (PalanuOS texts). */
pal_rect_t kui_dialog(pal_canvas_t *dst, pal_rect_t area, const char *title, const char *body,
                      const char *const *buttons, int nbuttons, int selected, bool wrap);
/* The reference Toast: a full-width ink band with rounded (radius 1) corners at the box's top. The reference draws the caption in ink (it
 * vanishes on the band); PalanuOS draws it in paper so the text is readable. */
/* The reference Dialog buttons: a centred row of bordered boxes (label + 4 px each side, 4 px apart) at (x, y) in a w-wide column.
 * Returns the row's height (the label cell + 8). sel is the inverted one. */
int kui_buttons(kui_canvas_t *c, int x, int y, int w, const char *const *labels, int n, int sel);
void kui_toast_band(pal_canvas_t *dst, pal_rect_t box, const char *text);
/* The reference Popup: text centred in the 144 px inside a 160 px column, its top at y (logical; the column's padding is included). */
void kui_popup(kui_canvas_t *c, int y, const char *text);
/* The reference list screen (ListSection + ListContainer rows, FileOpsModal's confirm): the section title at (x, y) in Bold8, then the
 * rows from first on a 17 px pitch, inset 2 px, in the list box below it. Returns the logical y under the last row. */
int kui_list_page(kui_canvas_t *c, int x, int y, int w, int h, const char *title, const char *const *labels, int n, int sel,
                  int first);

#endif
