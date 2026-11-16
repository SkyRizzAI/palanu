/* PalanuOS menu pages: a page is a title and a list of rows, pages nest on a stack, and each row is data (a kind plus
 * getter, setter and callback pointers with a ctx). Settings uses it; any app can use it for the same list look (title
 * bar, rows with 16 px icons, the selected row inverted, section headers, value on the right).
 *
 * Row kinds:
 *   SECTION    a header in small dim capitals; never selected
 *   SUBMENU    chevron; Enter pushes its page (the page stays remembered: selection and scroll per page)
 *   TOGGLE     on/off switch; Enter or Fn+A/D flips it
 *   CHOICE     value with < >; Fn+A/D cycles, Enter opens a picker
 *   SLIDER     bar with value; Fn+A/D steps, Shift+Fn+A/D steps by four times the step (no set = read-only gauge: a
 *              bar, not focusable, its value is the percentage in min..max)
 *   ACTION     Enter runs it; with confirm set it asks first in a dialog
 *   INFO       read-only value; wrap puts a long value on its own lines
 *   TEXT       value on the right; Enter opens the virtual keyboard (pal_vk_edit_async), masked when mask is set
 *
 * Esc pops a page; at the root Esc is not consumed, so the desktop returns home. Dynamic pages set build: it fills the
 * page through pal_menu_add on every push and on pal_menu_refresh. Asking (picker, dialog, text) goes through hooks, so a
 * host test can answer them without a desktop (pal_menu_set_hooks). Nothing here allocates. */
#ifndef PAL_MENU_H
#define PAL_MENU_H

#include "pal/kui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"
#include "pal/gfx.h"
#include "pal/input.h"
#include "pal/theme.h"

#define PAL_MENU_DEPTH 6       /* pages open at once */
#define PAL_MENU_TEXT_MAX 65   /* longest text a TEXT row or pal_menu_ask_text can enter, NUL included */
#define PAL_MENU_VALUE_MAX 160 /* longest value text a row callback writes, NUL included (INFO rows may wrap) */

typedef enum {
    PAL_MENU_SECTION = 0,
    PAL_MENU_SUBMENU,
    PAL_MENU_TOGGLE,
    PAL_MENU_CHOICE,
    PAL_MENU_SLIDER,
    PAL_MENU_ACTION,
    PAL_MENU_INFO,
    PAL_MENU_TEXT,
} pal_menu_kind_t;

typedef struct pal_menu_page pal_menu_page_t;
typedef struct pal_menu_row pal_menu_row_t;
typedef struct pal_menu pal_menu_t;

struct pal_menu_row {
    pal_menu_kind_t kind;
    const char *label;
    const char *icon;      /* a 16x16 icon name (assets/icons), or NULL */
    const char *hint;      /* one line under the list while the row is selected, or NULL */
    void *ctx;             /* passed to every callback below */
    bool danger;           /* ACTION: drawn as destructive (accent label) */
    bool hidden;           /* left out of the page (for example: the board lacks the hardware) */
    bool disabled;         /* selectable and drawn like the others, but its keys do nothing but say why: Enter toasts the hint
                            * (or "Not available now"), so a row that cannot be used never looks like a skipped one */
    bool inert;            /* a text row (the reference info row): never selected, the selection skips it */
    /* The value. TOGGLE: 0 or 1. CHOICE: the index into choices. SLIDER: the value in min..max. */
    int (*get)(void *ctx);
    void (*set)(void *ctx, int v); /* NULL on a SLIDER makes it a read-only gauge */
    const char *const *choices;    /* CHOICE: labels, NULL-terminated */
    int min, max, step;            /* SLIDER */
    /* The value text: INFO, TEXT, SUBMENU and ACTION (optional). Writes at most PAL_MENU_VALUE_MAX bytes. */
    void (*text)(void *ctx, char *out, size_t cap);
    pal_menu_page_t *(*open)(void *ctx); /* SUBMENU: the page to push, or NULL */
    void (*run)(void *ctx);              /* ACTION */
    const char *confirm;                 /* ACTION: ask first with this text (OK / Cancel) */
    const char *ok_label;                /* ACTION with confirm: the OK button (NULL = "OK") */
    bool (*apply_text)(void *ctx, const char *text); /* TEXT: store the entered text; false shows "Not saved" */
    bool mask;                           /* TEXT: masked entry */
    int max_len;                         /* TEXT: longest entry (0 = PAL_MENU_TEXT_MAX - 1) */
    const char *title;                   /* TEXT: title on the keyboard (NULL = label) */
    bool wrap;                           /* INFO: the value wraps below the label */
    bool show_value;                     /* ACTION: draw the text value on the right (the reference Wi-Fi network rows) */
};

struct pal_menu_page {
    const char *title;
    const pal_menu_row_t *rows; /* the rows; a build points it at store */
    size_t n;
    pal_menu_row_t *store;      /* storage a build fills with pal_menu_add (NULL: static page) */
    size_t cap;
    void (*build)(pal_menu_page_t *p); /* dynamic pages: fills p; runs on push and on pal_menu_refresh */
    void *ctx;
    const char *empty;          /* text shown when the page has no rows */
    bool live;                  /* redraw every 500 ms, for values that move (uptime, battery) */
    bool content_bar;           /* the scrollbar shows when the rows are taller than the list (the reference Wi-Fi list); by
                                 * default it shows only when rows are cut off, as on the settings pages */
    int sel, scroll;            /* kept per page while it is on the stack */
    int spx;                    /* the drawn list's scroll in pixels (menu_draw.c, the reference ScrollState) */
    bool follow;                /* the selection changed (or the page opened): the next layout scrolls it into view. Only then:
                                 * a key that scrolls the page to read the rows between two selectable ones must not be undone
                                 * by the layout on the next frame */
};

/* Hooks for the asking calls. NULL members use the desktop (dialogs, pickers, the virtual keyboard, toasts). A test
 * replaces them, records the request and answers it with pal_menu_answer_pick / _confirm / _ask. */
typedef struct {
    void (*picker)(pal_menu_t *m, const char *title, const char *const *items, int n);
    void (*confirm)(pal_menu_t *m, const char *title, const char *text, const char *ok);
    void (*ask)(pal_menu_t *m, const char *title, const char *initial, int max_len, bool mask);
    void (*toast)(const char *text);
} pal_menu_hooks_t;

typedef enum {
    PAL_MENU_PEND_NONE = 0,
    PAL_MENU_PEND_PICK,
    PAL_MENU_PEND_CONFIRM,
    PAL_MENU_PEND_ASK,
} pal_menu_pend_t;

struct pal_menu {
    pal_menu_page_t *stack[PAL_MENU_DEPTH];
    int depth;
    /* The one question open on the desktop: a row's (pend_page, pend_row) or a caller's done callback. */
    pal_menu_pend_t pend;
    pal_menu_page_t *pend_page;
    size_t pend_row;
    void (*done_pick)(void *ctx, int index);
    void (*done_confirm)(void *ctx, bool ok);
    void (*done_ask)(void *ctx, bool ok, const char *text);
    void *done_ctx;
};

void pal_menu_set_hooks(const pal_menu_hooks_t *h); /* NULL restores the desktop */
/* Answers for the open question (a test, or the desktop callbacks). Only the answer for the pending kind has an effect. */
void pal_menu_answer_pick(pal_menu_t *m, int index);       /* -1: cancelled */
void pal_menu_answer_confirm(pal_menu_t *m, bool ok);
void pal_menu_answer_ask(pal_menu_t *m, bool ok, const char *text);

void pal_menu_init(pal_menu_t *m, pal_menu_page_t *root);
/* Makes root the only page on the stack (the Settings view does this on entry when it is not asked for a page). */
void pal_menu_reset(pal_menu_t *m, pal_menu_page_t *root);
bool pal_menu_push(pal_menu_t *m, pal_menu_page_t *p); /* false when the stack is full */
bool pal_menu_pop(pal_menu_t *m);                       /* false at the root */
pal_menu_page_t *pal_menu_top(const pal_menu_t *m);
int pal_menu_depth(const pal_menu_t *m);

/* Dynamic pages: clear and add rows from a build. add returns false when the store is full. */
void pal_menu_clear(pal_menu_page_t *p);
bool pal_menu_add(pal_menu_page_t *p, const pal_menu_row_t *r);
/* Rebuilds the top page (after a change that alters its rows) and keeps the selection inside it. */
void pal_menu_refresh(pal_menu_t *m);

/* Keys. True when the key was used (the caller redraws). Esc at the root returns false.
 * Up and Down move the selection over the selectable rows (wrapping), and the page scrolls to keep it in view: its section
 * headers come with it, and the first and last selectable rows bring the rows before and after them, so nothing on a page is
 * out of reach. A page with no selectable row at all (information only) is scrolled instead: Up and Down by a row, Left and
 * Right by a page. */
bool pal_menu_key(pal_menu_t *m, const pal_key_t *k);

/* Selection helpers (also for tests). */
bool pal_menu_select(pal_menu_t *m, size_t index);     /* false when index is not a selectable row */
pal_menu_row_t *pal_menu_selected(pal_menu_t *m);     /* NULL when none */
/* Activates the selected row as Enter would (tests and the Settings walk). */
bool pal_menu_activate(pal_menu_t *m);
/* A value change through the keys: step is -1 or +1 (Shift gives a big step to sliders). */
bool pal_menu_step(pal_menu_t *m, int dir, bool big);

/* Asking helpers: the same as the row kinds use. done runs on the desktop task when the answer arrives. */
void pal_menu_ask_text(pal_menu_t *m, const char *title, const char *initial, int max_len, bool mask,
                       void (*done)(void *ctx, bool ok, const char *text), void *ctx);
void pal_menu_confirm(pal_menu_t *m, const char *title, const char *text, const char *ok,
                      void (*done)(void *ctx, bool ok), void *ctx);
void pal_menu_toast(const char *text);

/* Drawing. Pages are drawn into a, title bar at the top. Key legends are the legend bar (pal/legend.h), not the page's. */
void pal_menu_draw(pal_menu_t *m, pal_canvas_t *c, pal_rect_t a, const pal_theme_t *t);
/* The rows that fit below the title in h pixels, starting at the page's scroll (keeps the selection visible). */
void pal_menu_layout(pal_menu_t *m, const pal_theme_t *t, int w, int list_h, int *first, int *count);
/* Scrolls the page so the selection is in view (after a key moves it; drawing does the same). */
void pal_menu_follow(pal_menu_t *m);
/* The height of row i on the page at width w (sections, wrapped info rows and the others differ). */
int pal_menu_row_h(const pal_menu_page_t *p, size_t i, const pal_theme_t *t, int w);

/* Before a selection move: when the rows between the selection and the next selectable one (dir +1 down, -1 up; past the last
 * selectable row, to the end of the page) have text that is not in view yet, scrolls the page one step toward it and returns
 * true (the key is used up). Pressing Down again and again therefore reads a long block of information rows before it
 * reaches the next selectable row. A section header alone between two rows does not count: it comes with the row below it. */
bool pal_menu_reveal(pal_menu_t *m, int dir);
/* Scrolls the page's list by a row (dir -1 up, +1 down) or by a page, without moving the selection. True when it moved.
 * What pal_menu_key does on a page with nothing to select. */
bool pal_menu_scroll(pal_menu_t *m, int dir, bool page);
/* The page has a row the selection can stop on. */
bool pal_menu_has_selectable(const pal_menu_page_t *p);
/* Long text: the selected row's label or value that does not fit scrolls sideways (after a short pause) instead of being cut
 * with "...". Call every 250 ms; true when the page needs a repaint. */
bool pal_menu_tick(pal_menu_t *m);

/* Kind helpers. */
bool pal_menu_selectable(const pal_menu_row_t *r);
bool pal_menu_gauge(const pal_menu_row_t *r); /* a SLIDER without a setter: a read-only bar (the reference ProgressBar) */

/* The 8 px icon of a name (menu_draw.c): its 8 px bitmap, or the 16 px one sampled down. Drawn in ink; a selected row inverts
 * it with the row (kui_row_frame). Settings rows and the notification center use it. */
void pal_menu_icon8(kui_canvas_t *k, const char *name, int x, int y);

#endif
