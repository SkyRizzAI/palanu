/* Internal state of the on-screen keyboard, shared by vk.c (the controller) and vk_draw.c (the drawing). */
#ifndef PAL_VK_INT_H
#define PAL_VK_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/nema.h"
#include "pal/gfx.h"
#include "pal/vk.h"
#include "pal/font.h"
#include "pal/vk_layout.h"

#define VK_TEXT_MAX 256
#define VK_SOURCES 8

typedef struct {
    /* keyboard state */
    bool open;
    bool fixed;        /* the box shows fixed text and does not follow the app (scenes, tests) */
    bool allow_empty;  /* entry mode */
    bool typed;        /* the last key was typed on the physical keyboard: Enter then sends or confirms; after the arrows it presses the focused key */
    vk_ui_t ui;
    vk_cell_t cells[VK_MAX_CELLS];
    int ncells, focus, pref2;
    uint8_t phys;      /* Shift/Ctrl/Alt held on the physical keyboard, as last seen */
    /* the box */
    char text[VK_TEXT_MAX + 1];
    size_t len, cursor, max; /* max: the most bytes the box takes */
    char sess[VK_TEXT_MAX + 1]; /* what was typed in this session (live mode without a source) */
    size_t sess_len;
    size_t scroll;     /* first visible code point */
    size_t cursor_cp;  /* the caret, in code points (set by the drawing) */
    char title[40];
    /* feedback and timers */
    int flash;         /* cell flashing after a press, -1 none */
    uint32_t flash_until, now, caret_at, last_key_ms, last_poll, fast_until;
    bool caret_on;
    /* redraw */
    bool need_full, dirty_text, dirty_caret;
    bool dirty_keys[VK_MAX_CELLS];
    /* entry completion */
    pal_vk_entry_t *ent;
    void (*cb)(int len, void *ctx);
    void *ctx;
    /* sources */
    pal_vk_source_t *src[VK_SOURCES];
    const char *src_id[VK_SOURCES];
    /* requests from other tasks */
    nema_port_mutex_t *lock;
    bool req_pending;
    int req_kind;
    char req_text[VK_TEXT_MAX + 1];
    char req_layer[8];
    pal_vk_entry_t *req_ent;
    void (*req_cb)(int len, void *ctx);
    void *req_ctx;
    /* the desktop task (pump runs there) and blocking callers */
    nema_tctx_t *desktop_ctx;
    bool blk_done;
    int blk_len;
    nema_obj_t done;
    bool done_init;
    pal_vk_hooks_t hooks;
} vk_state_t;

extern vk_state_t vk_S;

/* Tests and the drawing: a press of keyboard cell idx. */
void vk_press(int idx);

/* Drawing (vk_draw.c). full: the whole area; otherwise only what is marked dirty. Clears the marks. */
void vk_draw(pal_canvas_t *c, pal_rect_t a, bool full);

#endif
