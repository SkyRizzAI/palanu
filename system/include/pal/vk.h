/* PalanuOS on-screen keyboard (docs/VKEYBOARD.md): a full-page keyboard over the front view, and a text-entry screen for
 * system flows (the Wi-Fi password). It is an overlay (pal/overlay.h). Two modes:
 *
 *  - live: every key press is delivered to the front view at once, the way the physical keyboard would send it; the box
 *    at the top shows the line the app is editing (a text source), or what was typed in this session.
 *  - entry: nothing goes to the app; the box is the text being entered; OK (Enter) returns it, Esc cancels.
 */
#ifndef PAL_VK_H
#define PAL_VK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"
#include "pal/desktop.h"
#include "pal/input.h"

#define PAL_VK_CANCEL (-1) /* pal_vk_edit: the user cancelled. Errors are negative nema_err_t values below -1. */

/* A view that edits a line registers a source under its view id; the box follows it. get_text copies the line (bytes) and
 * its cursor (byte offset) and returns the length, or -1 when no line is being edited right now. */
typedef struct {
    int (*get_text)(void *ctx, char *out, size_t cap, size_t *cursor);
    void *ctx;
} pal_vk_source_t;

/* Entry mode. The text is copied to buf (NUL-terminated, cap bytes; at most cap-1 bytes are entered). */
typedef struct {
    const char *title;   /* the chip beside the box, e.g. "Wi-Fi password" */
    const char *initial; /* the text at the start; may be NULL */
    char *buf;
    size_t cap;
    bool mask;        /* show dots; the eye key reveals the text */
    bool allow_empty; /* OK with an empty text is refused when false */
} pal_vk_entry_t;

/* Test seam: replaces the desktop calls (overlay push and pop, presence, key delivery, clock, wake-up). NULL members use the
 * desktop. Pass NULL to the setter to restore the desktop. */
typedef struct {
    nema_err_t (*push)(pal_view_t *v);
    void (*pop)(pal_view_t *v);
    bool (*present)(void);
    void (*inject)(const pal_key_t *k);
    uint32_t (*clock)(void);
    void (*wake)(void);
} pal_vk_hooks_t;

extern pal_view_t pal_vk_view;

void pal_vk_register(void);                                              /* boot: registers the view (desktop running) */
nema_err_t pal_vk_register_source(const char *view_id, pal_vk_source_t *src);

/* Live keyboard. From any task: queued for the desktop task (NEMA_OK means queued). */
nema_err_t pal_vk_open(void);
void pal_vk_close(void);
void pal_vk_toggle(void);     /* desktop task only: the Fn double tap (PAL_KEY_VKB) */
bool pal_vk_is_open(void);
/* Live keyboard with fixed text and a start layer (abc, ABC, 123, #+=, ctl): for scenes and tests. */
nema_err_t pal_vk_open_text(const char *text, const char *layer);

/* Entry. pal_vk_edit blocks the calling process until OK (returns the length, >= 0) or cancel (PAL_VK_CANCEL). It returns
 * a negative nema_err_t when there is no display (NEMA_ERR_NOSYS: the caller falls back to a no-echo prompt), when it is
 * called from the desktop task (NEMA_ERR_PERM: use the async form there), or when another keyboard is open (NEMA_ERR_BUSY). */
int pal_vk_edit(pal_vk_entry_t *e);
/* The same for the desktop task and other non-blocking callers: cb(len, ctx) runs on the desktop task when it finishes
 * (len < 0: cancelled). */
nema_err_t pal_vk_edit_async(pal_vk_entry_t *e, void (*cb)(int len, void *ctx), void *ctx);

void pal_vk_set_hooks(const pal_vk_hooks_t *h);

#endif
