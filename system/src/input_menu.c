/* PalanuOS input menu (pal/input_menu.h): a modal box of five rows over the screen in front, drawn like the reference permission
 * screen (a dithered backdrop from the status bar's bottom, a rounded box, list rows with the focused one inverted). */
#include <string.h>

#include "desktop_int.h"
#include "nema/nema.h"
#include "pal/desktop.h"
#include "pal/input.h"
#include "pal/input_menu.h"
#include "pal/kui.h"
#include "pal/legend.h"
#include "pal/overlay.h"
#include "pal/vk.h"

enum { E_KEYBOARD, E_SYMBOLS, E_NUMBERS, E_CAPS, E_CTRL, E_COUNT };

static const struct {
    const char *label; /* the letter that picks the row, then its name */
    char hot;
    int mode; /* pal_input_mode_t, or -1 for the on-screen keyboard */
} ENT[E_COUNT] = {
    {"K  Keyboard", 'k', -1},
    {"S  Symbols", 's', PAL_INPUT_SYM},
    {"N  Numbers", 'n', PAL_INPUT_NUM},
    {"C  Caps Lock", 'c', PAL_INPUT_CAPS},
    {"T  Ctrl", 't', PAL_INPUT_CTRL},
};

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full);
static bool key(pal_view_t *v, const pal_key_t *k);
static void enter(pal_view_t *v);
static void leave(pal_view_t *v);

static const pal_legend_item_t LEGENDS[] = {{PAL_ACT_MOVE_V, "move", NULL}, {PAL_ACT_OK, "pick", NULL}, {PAL_ACT_BACK, "close", NULL}};

static struct {
    pal_view_t view;
    int sel, top, vis;
} M = {.view = {.id = "input-menu", .title = "Input", .flags = 0, .draw = draw, .key = key, .enter = enter, .leave = leave, .data = &M, .legends = LEGENDS,
                .nlegends = 3}};

static kui_canvas_t S NEMA_EXT_BSS;

/* While the menu is up a single tap of Fn is Down (PAL_KEY_FNTAP) and two quick taps close it (PAL_KEY_VKB, like Esc). The input
 * service sends the first only when asked. */
static void enter(pal_view_t *v) {
    (void)v;
    pal_input_report_fn_tap(true);
}

static void leave(pal_view_t *v) {
    (void)v;
    pal_input_report_fn_tap(false);
}

static const char *state(int e) {
    if (ENT[e].mode < 0) return NULL;
    return pal_input_mode((pal_input_mode_t)ENT[e].mode) ? "On" : "Off";
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&S, c->w, c->h, kui_scale_q());
    const int ay = kui_lg(&S, a.y), ab = kui_lg(&S, a.y + a.h);
    kui_backdrop(c, a, ay, S.sq);
    int fit = (ab - ay - 6) / KUI_LIST_PITCH; /* rows that fit, at least one: the box scrolls when the screen is small */
    if (fit < 1) fit = 1;
    M.vis = fit > E_COUNT ? E_COUNT : fit;
    if (M.sel < M.top) M.top = M.sel;
    if (M.sel >= M.top + M.vis) M.top = M.sel - M.vis + 1;
    const int mw = S.w, mh = M.vis * KUI_LIST_PITCH + 2; /* the width of the screen, like the reference permission box (210 clamped to the canvas) */
    const int mx = (S.w - mw) / 2, my = ay + ((ab - ay) - mh) / 2;
    kui_round(&S, mx, my, mw, mh, 1, true);
    for (int i = 0; i < M.vis; i++) {
        int e = M.top + i;
        kui_list_item_t it = {.label = ENT[e].label, .icon = -1, .value = state(e), .selected = e == M.sel};
        kui_list_item(&S, mx + 2, my + 2 + i * KUI_LIST_PITCH, mw - 4, &it);
    }
    kui_present(c, &S, (pal_rect_t){kui_ph(S.sq, mx), kui_ph(S.sq, my), kui_ph(S.sq, mx + mw) - kui_ph(S.sq, mx),
                                    kui_ph(S.sq, my + mh) - kui_ph(S.sq, my)});
}

/* Desktop task. Closes the menu, then does the row: a mode flips, the keyboard opens. */
static void pick(int e) {
    ovl_remove_now(&M.view);
    if (ENT[e].mode < 0) {
        pal_vk_open();
        return;
    }
    pal_input_mode_t m = (pal_input_mode_t)ENT[e].mode;
    pal_input_set_mode(m, !pal_input_mode(m));
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    switch (k->sym) {
    case PAL_KEY_UP:
    case PAL_KEY_LEFT: M.sel = (M.sel + E_COUNT - 1) % E_COUNT; break;
    case PAL_KEY_DOWN:
    case PAL_KEY_RIGHT:
    case PAL_KEY_FNTAP: M.sel = (M.sel + 1) % E_COUNT; break; /* one tap of Fn is Down */
    case PAL_KEY_ENTER: pick(M.sel); return true;
    case PAL_KEY_VKB: /* Fn tapped twice closes, as Esc does */
    case PAL_KEY_ESC: ovl_remove_now(&M.view); return true;
    case PAL_KEY_CHAR:
        for (int e = 0; e < E_COUNT; e++)
            if (k->cp == (uint32_t)ENT[e].hot || k->cp == (uint32_t)ENT[e].hot - 32u) {
                pick(e);
                return true;
            }
        return true;
    default: return true; /* a modal: it takes every key */
    }
    pal_desktop_invalidate();
    return true;
}

nema_err_t pal_input_menu_open(void) {
    M.sel = 0; /* always from the on-screen keyboard: the rows are then reached by counting */
    M.top = 0;
    return pal_desktop_overlay_push(&M.view);
}

void pal_input_menu_fn_double(void) {
    if (pal_vk_is_open()) {
        pal_vk_toggle();
        return;
    }
    if (pal_desktop_overlay_top() == &M.view) {
        ovl_remove_now(&M.view); /* Fn Fn opens the menu and closes it again, as Esc does */
        return;
    }
    pal_input_menu_open();
}
