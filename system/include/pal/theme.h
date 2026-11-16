/* PalanuOS themes: tokens, a palette, and a table of replaceable components.
 *
 * The system draws status bars, titles, and list rows through the active theme's component table (the legend bar is not
 * themed: pal/legend.h).
 * A theme may replace any single component; the rest fall back to the defaults, so tema variety does not
 * cost app code anything. Colours are palette indices: the semantic ones below live in the top eight slots,
 * the rest follow xterm's 256-colour layout so terminal programs look right.
 */
#ifndef PAL_THEME_H
#define PAL_THEME_H

#include "pal/gfx.h"

#define PAL_C_ON_ACCENT 248 /* text drawn on the accent colour */
#define PAL_C_TERM_BG 249   /* terminal default background */
#define PAL_C_TERM_FG 250   /* terminal default foreground */
#define PAL_C_PANEL 251     /* secondary surface */
#define PAL_C_DIM 252       /* muted text */
#define PAL_C_ACCENT 253
#define PAL_C_PAPER 254     /* background */
#define PAL_C_INK 255       /* text */

typedef struct {
    uint8_t pad_x, pad_y, gap, radius;
    const pal_font_t *title, *body, *caption;
    bool title_bold;
} pal_tokens_t;

typedef struct {
    uint32_t paper, ink, accent, dim, panel, on_accent, term_bg, term_fg; /* 0xRRGGBB */
} pal_theme_colors_t;

typedef struct {
    int hour, minute;
    int battery; /* 0..100, -1 unknown */
    int wifi;    /* 0 off, 1..4 signal bars */
    bool link;   /* a remote client is connected */
    bool ble;    /* Bluetooth is on (appended later) */
    bool sd;     /* a card is in the slot and mounted (appended later) */
} pal_status_t;

typedef struct pal_theme pal_theme_t;
typedef struct {
    int (*status_bar)(pal_canvas_t *c, const pal_theme_t *t, const pal_status_t *st);
    int (*title_bar)(pal_canvas_t *c, const pal_theme_t *t, int y, const char *title);
    int (*row_height)(const pal_theme_t *t);
    void (*list_row)(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int w, const char *label,
                     const char *value, bool selected);
    void (*scrollbar)(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int h, int offset, int visible,
                      int total);
    /* Appended later. Every theme inherits the defaults for any member it leaves NULL (system/src/theme_widgets.c). */
    /* Modal frame centred in area: title strip, wrapped text, buttons (selected one inverted; buttons may be NULL).
     * Returns the frame. */
    pal_rect_t (*dialog_box)(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t area, const char *title, const char *text,
                             const char *const *buttons, int nbuttons, int selected);
    /* One-line text field at (x, y), w pixels wide. shown is the visible text (masked and scrolled already); caret is the
     * caret column in characters from the start of shown. Returns its height. */
    int (*text_field)(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int w, const char *shown, int caret,
                      bool caret_on);
    /* Toast bar inside box (inverted, rounded), text in the caption font. */
    void (*toast)(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t box, const char *text);
    /* Spinner of eight dots around (cx, cy), radius 5; frame lights one dot (frame modulo 8). */
    void (*spinner)(pal_canvas_t *c, const pal_theme_t *t, int cx, int cy, int frame);
} pal_components_t;

struct pal_theme {
    const char *name;
    pal_tokens_t tokens;
    pal_theme_colors_t colors;
    pal_components_t comp;
};

extern const pal_components_t PAL_DEFAULT_COMPONENTS;

/* Settings: which theme, dark mode, and UI size (0 = small, 1 = medium). */
int pal_theme_count(void);
const char *pal_theme_name(int index);
void pal_theme_select(int index, bool dark, int size);
const pal_theme_t *pal_theme_active(void);
int pal_theme_index(void);
bool pal_theme_dark(void);
int pal_theme_size(void);

/* Fill a 256-entry RGB565 palette (byte-swapped for SPI) for the active theme. */
void pal_theme_palette(uint16_t out[256]);

#endif
