/* Athena's parts (athena.c, athena_lock.c, athena_page.c, athena_shell.c): the shared state and calls. */
#ifndef ATHENA_INT_H
#define ATHENA_INT_H

#include <stdio.h>
#include <string.h>

#include "../desktop_int.h"
#include "lvgl.h"
#include "nema/types.h"
#include "athena_pool.h"
#include "nema/nema.h"
#include "pal/console.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/display.h"
#include "pal/dpm.h"
#include "pal/input.h"
#include "pal/menu.h"
#include "pal/nav.h"
#include "pal/overlay.h"
#include "pal/registry.h"
#include "pal/time.h"
#include "pal/toast.h"
#include "pal/ui.h"
#include "pal/vk.h"

#define MAX_W 320
#define MAX_H 240
#define MAX_TILES 24
#define MAX_WIDGETS 4
#define MAX_ROWS 8
#define BAR_H 20
#define SK_H 20 /* the softkey bar, when the theme has one */
#define MAX_PAGE_ROWS 40
#define VALUE_MAX 40

/* The theme's tokens (athena_theme.c). Every colour and shape Athena draws comes from here. */
typedef struct {
    const char *id, *name;
    uint32_t wall_top, wall_bot; /* the home's wallpaper, top to bottom */
    uint32_t surface, row;       /* a card or page, and a row or control on it */
    uint32_t accent, ink, muted; /* the focus and buttons, the text, the secondary text */
    uint32_t on, danger, track;  /* a switch that is on, a destructive row, a switch that is off */
    uint32_t key_mod, bar_bg;    /* a keyboard's mode keys; the status bar over an app */
    int r_card, r_row, r_icon;   /* corner radii (LV_RADIUS_CIRCLE for round) */
    bool title_left, caps_buttons, flat;
    /* Layout: what the theme puts where, not only how it is coloured. Zero is the Cupertino and Material way. */
    uint32_t hl_top, hl_bot;     /* the focus fill as a gradient when they differ; otherwise the accent */
    uint32_t pane_top, pane_bot; /* the title pane and the softkey bar, top to bottom */
    bool softkeys;   /* a softkey bar at the bottom of every screen: Options (Tab), the Enter action, Back (Esc) */
    bool idle_home;  /* the home is an idle screen (the widgets); the apps are in a menu grid it opens */
    bool title_pane; /* a page's title sits on a band in the pane colours */
    bool level_bars; /* the status bar shows the signal and the battery as rising bars */
    uint8_t dock;      /* idle_home: a row of the first apps along the bottom of the idle screen, this many (0: none) */
    uint8_t menu_cols; /* idle_home: the menu grid's columns (0: 3) */
    bool menu_dark;    /* idle_home: the menu is icons on the wallpaper, no names; the selected app's name is the title */
    bool popup_right;  /* Options opens at the right edge (otherwise above the left softkey) */
} ath_theme_t;

extern const ath_theme_t *ath_T;
extern const char *const ATH_THEME_IDS[];
const ath_theme_t *ath_theme_wanted(void);

#define C_WALL_TOP lv_color_hex(ath_T->wall_top)
#define C_WALL_BOT lv_color_hex(ath_T->wall_bot)
#define C_WHITE lv_color_hex(0xFFFFFF)
#define C_BLACK lv_color_hex(0x000000)
#define C_SHEET lv_color_hex(ath_T->surface)
#define C_ROW lv_color_hex(ath_T->row)
#define C_BLUE lv_color_hex(ath_T->accent)
#define C_INK lv_color_hex(ath_T->ink)
#define C_GREY lv_color_hex(ath_T->muted)
#define C_GREEN lv_color_hex(ath_T->on)
#define C_RED lv_color_hex(ath_T->danger)
#define C_TRACK lv_color_hex(ath_T->track)

typedef enum { SHEET_NONE, SHEET_KEYBOARD, SHEET_SHORTCUTS, SHEET_RECENTS, SHEET_OPTIONS } sheet_t;

typedef struct {
    lv_display_t *disp;
    int w, h;
    /* status bar (on LVGL's top layer, above everything) */
    lv_obj_t *bar, *bar_time, *bar_right;
    lv_obj_t *lvl_sig[4], *lvl_bat[5]; /* level_bars: the signal and battery bars */
    bool bar_solid;
    pal_status_t st;
    /* home */
    lv_obj_t *home, *grid;
    lv_obj_t *menu;  /* idle_home: the app menu (a title pane over the grid); hidden on the idle screen */
    lv_obj_t *menu_title;
    bool menu_open;
    lv_obj_t *dock, *dock_name, *dtile[8]; /* the idle screen's row of apps (ath_theme_t.dock) */
    int nd, dsel;
    lv_obj_t *wbox[MAX_WIDGETS];
    const struct ath_widget *wdef[MAX_WIDGETS];
    int nw;
    uint8_t tcol[MAX_TILES], trow[MAX_TILES]; /* each app's place in the home's grid */
    lv_obj_t *tile[MAX_TILES];
    pal_reg_app_t app[MAX_TILES];
    int n, sel, cols, last_minute;
    /* sheet */
    lv_obj_t *sheet, *sheet_title, *sheet_hint;
    lv_obj_t *row[MAX_ROWS];
    char row_id[MAX_ROWS][24]; /* recents: the view each row opens */
    uint8_t row_act[MAX_ROWS]; /* options: the action each row runs */
    int nrows, row_sel;
    bool row_info; /* shortcuts: the rows are not choices */
    sheet_t kind;
    /* toolkit pages: a bound menu (pal_menu_bind) drawn as a grouped list */
    lv_obj_t *page, *page_pane, *page_title, *page_back, *page_list, *page_empty;
    lv_obj_t *prow[MAX_PAGE_ROWS], *pval[MAX_PAGE_ROWS], *pbar[MAX_PAGE_ROWS], *plabel[MAX_PAGE_ROWS], *picon[MAX_PAGE_ROWS];
    int pidx[MAX_PAGE_ROWS]; /* the page row each drawn row shows */
    char ptext[MAX_PAGE_ROWS][VALUE_MAX];
    int pn, psel;
    pal_menu_t *pm;
    pal_menu_page_t *ppage;
    size_t prows;
    uint32_t pvals_ms;
    int mode; /* 0 home, 1 toolkit page, 2 an app drawn by the compositor */
    /* the lock greeter (the session's own lock screen; the PIN is the system's) */
    lv_obj_t *lock, *lock_clock, *lock_date, *lock_dots, *lock_msg, *lock_hint;
    bool lock_shown, lock_dirty;
    pal_ui_pin_t pin;
    int enters;
    /* sending */
    uint32_t last_ms;
    bool pending, force, owned, display_on;
    uint32_t console_gen; /* pal_console_generation at the last frame */
    int dx0, dy0, dx1, dy1; /* dirty union of what LVGL has drawn since the last send */
    /* shell services and toasts (athena_shell.c) */
    lv_obj_t *backdrop, *dim, *modal, *bar_toast;
    uint16_t *snap;   /* the screen under a modal, copied from the system frame when it opens */
    uint32_t svc_sig; /* what the modal shows now */
    int svc_kind;
    bool toast_shown;
    /* the softkey bar (athena_softkeys.c) */
    lv_obj_t *sk, *sk_l, *sk_c, *sk_r;
} athena_t;

extern athena_t A;
extern uint16_t *s_frame;


lv_obj_t *ath_text(lv_obj_t *parent, const lv_font_t *f, lv_color_t c);
/* The focus fill on o (on), or its resting colour (off): the theme's highlight gradient, or the accent. */
void ath_focus_bg(lv_obj_t *o, bool on, lv_color_t off);
/* A band in the pane colours (the title pane, the softkey bar). */
void ath_pane_bg(lv_obj_t *o);
/* Rows the softkey bar takes at the bottom (0 without one). */
static inline int ath_bottom(void) { return ath_T->softkeys ? SK_H : 0; }

/* Home widgets (athena_widgets.c): a variant per theme; the home builds the ones ATH_HOME_WIDGETS names, in that order. */
typedef struct ath_widget {
    const char *id;           /* what the home asks for: "clock", "calendar" */
    const char *style;        /* the theme id it is for, NULL for every theme */
    uint8_t cols, rows;       /* its span on the home's grid */
    void (*build)(lv_obj_t *box, int w, int h);
    void (*tick)(lv_obj_t *box, const pal_tm_t *t); /* once a second while the home is in front */
} ath_widget_t;
extern const char *const ATH_HOME_WIDGETS[];
const ath_widget_t *ath_widget_find(const char *id);
void ath_date_text(const pal_tm_t *t, char *out, size_t cap);
bool ath_home_in_front(void);

/* the lock greeter (athena_lock.c) */
extern pal_view_t ath_lock_view;
void ath_build_lock(void);
void ath_lock_sync(void);

/* bound menus drawn as grouped lists (athena_page.c) */
pal_menu_t *ath_page_menu(void);
void ath_build_page(void);
void ath_page_sync(pal_menu_t *m, uint32_t now);

/* sheets and the Options popup (athena_sheet.c) */
void ath_sheet_geometry(void);
void ath_sheet_close(void);
void ath_sheet_move(int d);
void ath_sheet_choose(void);
void ath_open_keyboard_sheet(void);
void ath_open_shortcuts_sheet(void);
void ath_open_recents_sheet(void);
void ath_open_options(void);
void ath_menu_show(bool open); /* the idle home's app menu (athena.c) */

/* sheets and the Options popup (athena_sheet.c) */
void ath_sheet_geometry(void);
void ath_sheet_close(void);
void ath_sheet_move(int d);
void ath_sheet_choose(void);
void ath_open_keyboard_sheet(void);
void ath_open_shortcuts_sheet(void);
void ath_open_recents_sheet(void);
void ath_open_options(void);
void ath_menu_show(bool open); /* the idle home's app menu (athena.c) */

/* the softkey bar (athena_softkeys.c): three labels at the bottom, for the theme that has one */
void ath_softkeys_build(void);
void ath_softkeys_set(const char *left, const char *mid, const char *right);
void ath_softkeys_show(bool on);

/* shell services and toasts (athena_shell.c, pal/shell.h) */
void ath_shell_build(void);
bool ath_shell_active(void);   /* a service is in front: Athena draws it as a modal */
void ath_shell_sync(void);     /* follows the service in front and the toast */

#endif
