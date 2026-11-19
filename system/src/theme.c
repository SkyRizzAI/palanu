#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/kui.h"
#include "pal/theme.h"
#include "theme_widgets.h"
#include "pal/ui.h"

/* ---------------------------------------------------------------- default components */
/* The status bar is the reference (kui_status.c): the palette's ink fills it and the clock, battery and Wi-Fi are drawn in paper. */
static kui_canvas_t S_BAR NEMA_EXT_BSS;

static int kui_status_bar(pal_canvas_t *c, const pal_theme_t *t, const pal_status_t *st) {
    (void)t;
    kui_status_t k = {pal_ui_hour(st->hour), st->minute, st->battery, st->wifi > 0}; /* 12 hours: no room for AM/PM here */
    kui_canvas_init(&S_BAR, c->w, c->h, kui_scale_q());
    kui_clear(&S_BAR, false);
    kui_status_draw(&S_BAR, &k);
    kui_present(c, &S_BAR, (pal_rect_t){0, 0, c->w, kui_top_px(KUI_STATUS_H)});
    return kui_top_px(KUI_STATUS_H);
}

static int def_title_bar(pal_canvas_t *c, const pal_theme_t *t, int y, const char *title) {
    const pal_font_t *f = t->tokens.title;
    int h = f->h + 4;
    pal_fill_round(c, 2, y + 2, c->w - 4, h, t->tokens.radius, PAL_C_INK);
    int x = (c->w - pal_text_w(f, title)) / 2;
    pal_text(c, f, x, y + 4, title, PAL_C_PAPER, PAL_TRANSPARENT);
    if (t->tokens.title_bold) pal_text(c, f, x + 1, y + 4, title, PAL_C_PAPER, PAL_TRANSPARENT);
    return h + 4;
}

static int def_row_height(const pal_theme_t *t) { return t->tokens.body->h + 2 * t->tokens.pad_y; }

static void def_list_row(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int w, const char *label,
                         const char *value, bool selected) {
    int h = def_row_height(t);
    const pal_font_t *f = t->tokens.body;
    pal_color_t fg = selected ? PAL_C_PAPER : PAL_C_INK;
    if (selected) pal_fill_round(c, x, y, w, h, t->tokens.radius, PAL_C_INK);
    pal_text(c, f, x + t->tokens.pad_x, y + t->tokens.pad_y, label, fg, PAL_TRANSPARENT);
    if (value)
        pal_text(c, f, x + w - t->tokens.pad_x - pal_text_w(f, value), y + t->tokens.pad_y, value, fg,
                 PAL_TRANSPARENT);
}

static void def_scrollbar(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int h, int offset, int visible,
                          int total) {
    for (int i = 0; i < h; i += 2) pal_pixel(c, x + 1, y + i, PAL_C_DIM);
    int th = h * visible / total;
    if (th < 4) th = 4;
    int ty = total > visible ? y + (h - th) * offset / (total - visible) : y;
    pal_fill(c, x, ty, 3, th, PAL_C_INK);
}

const pal_components_t PAL_DEFAULT_COMPONENTS = {
    .status_bar = kui_status_bar,
    .title_bar = def_title_bar,
    .row_height = def_row_height,
    .list_row = def_list_row,
    .scrollbar = def_scrollbar,
    .dialog_box = pal_def_dialog_box,
    .text_field = pal_def_text_field,
    .toast = pal_def_toast,
    .spinner = pal_def_spinner,
};

/* ---------------------------------------------------------------- themes */
#define F5 (&PAL_FONT_SPLEEN_5X8)
#define F6 (&PAL_FONT_SPLEEN_6X12)
#define F8 (&PAL_FONT_SPLEEN_8X16)

typedef struct {
    const char *name;
    pal_theme_colors_t light, dark;
    pal_components_t comp;
    uint8_t radius;
} theme_def_t;

/* The palettes are the reference (kui_style.c): palanu is the default. Dark mode swaps paper and ink. */
static const theme_def_t DEFS[] = {
    {"palanu",
     {0xAD9EEF, 0x292442, 0x292442, 0x6A6388, 0xC9BDF2, 0xAD9EEF, 0x15122A, 0xE9E4FB},
     {0x292442, 0xAD9EEF, 0xAD9EEF, 0xA39CC4, 0x2C2552, 0x292442, 0x15122A, 0xE9E4FB},
     {0}, 2},
    {"mono",
     {0xFFFFFF, 0x000000, 0x000000, 0x707070, 0xE0E0E0, 0xFFFFFF, 0x000000, 0xE0E0E0},
     {0x000000, 0xFFFFFF, 0xFFFFFF, 0x909090, 0x202020, 0x000000, 0x000000, 0xE0E0E0},
     {0}, 1},
    {"orange",
     {0xFF6500, 0x000000, 0x000000, 0x804000, 0xFF8A3D, 0xFF6500, 0x000000, 0xFF6500},
     {0x000000, 0xFF6500, 0xFF6500, 0x804000, 0x202020, 0x000000, 0x000000, 0xFF6500},
     {0}, 1},
};

static struct {
    int index, size;
    bool dark;
    pal_theme_t active;
} S = {0, 1, false, {0}};

static void rebuild(void) {
    const theme_def_t *d = &DEFS[S.index];
    pal_theme_t *t = &S.active;
    t->name = d->name;
    t->colors = S.dark ? d->dark : d->light;
    t->comp = d->comp.status_bar ? d->comp : PAL_DEFAULT_COMPONENTS;
    pal_components_fill(&t->comp); /* a theme inherits every member it leaves NULL */
    t->tokens.radius = d->radius;
    if (S.size == 0) {
        t->tokens.caption = F5;
        t->tokens.body = F6;
        t->tokens.title = F8;
        t->tokens.pad_x = 4;
        t->tokens.pad_y = 2;
        t->tokens.gap = 2;
    } else {
        t->tokens.caption = F6;
        t->tokens.body = F8;
        t->tokens.title = F8;
        t->tokens.pad_x = 6;
        t->tokens.pad_y = 2;
        t->tokens.gap = 3;
    }
    t->tokens.title_bold = true;
}

int pal_theme_count(void) { return (int)(sizeof(DEFS) / sizeof(DEFS[0])); }
const char *pal_theme_name(int i) { return DEFS[i % pal_theme_count()].name; }

void pal_theme_select(int index, bool dark, int size) {
    S.index = ((index % pal_theme_count()) + pal_theme_count()) % pal_theme_count();
    S.dark = dark;
    S.size = size ? 1 : 0;
    rebuild();
}

const pal_theme_t *pal_theme_active(void) {
    if (!S.active.name) rebuild();
    return &S.active;
}
int pal_theme_index(void) { return S.index; }
bool pal_theme_dark(void) { return S.dark; }
int pal_theme_size(void) { return S.size; }

/* ---------------------------------------------------------------- palette */
static uint16_t rgb565_swapped(uint32_t rgb) {
    uint16_t v = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    return (uint16_t)((v >> 8) | (v << 8));
}

static struct {
    bool on;
    uint32_t bg, fg;
} s_term;

void pal_theme_term_override(bool on, uint32_t bg, uint32_t fg) { s_term.on = on, s_term.bg = bg, s_term.fg = fg; }

void pal_theme_palette(uint16_t out[256]) {
    static const uint32_t ansi[16] = {
        0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
        0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
    };
    for (int i = 0; i < 16; i++) out[i] = rgb565_swapped(ansi[i]);
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 0; i < 216; i++) {
        uint32_t rgb = ((uint32_t)lv[i / 36] << 16) | ((uint32_t)lv[(i / 6) % 6] << 8) | (uint32_t)lv[i % 6];
        out[16 + i] = rgb565_swapped(rgb);
    }
    for (int i = 0; i < 24; i++) {
        uint32_t g = (uint32_t)(8 + i * 10);
        out[232 + i] = rgb565_swapped((g << 16) | (g << 8) | g);
    }
    const pal_theme_colors_t *c = &pal_theme_active()->colors;
    out[PAL_C_ON_ACCENT] = rgb565_swapped(c->on_accent);
    out[PAL_C_TERM_BG] = rgb565_swapped(s_term.on ? s_term.bg : c->term_bg);
    out[PAL_C_TERM_FG] = rgb565_swapped(s_term.on ? s_term.fg : c->term_fg);
    out[PAL_C_PANEL] = rgb565_swapped(c->panel);
    out[PAL_C_DIM] = rgb565_swapped(c->dim);
    out[PAL_C_ACCENT] = rgb565_swapped(c->accent);
    out[PAL_C_PAPER] = rgb565_swapped(c->paper);
    out[PAL_C_INK] = rgb565_swapped(c->ink);
}
