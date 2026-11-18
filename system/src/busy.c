/* PalanuOS busy overlay: a spinner and a line of text in a small frame. It takes every key, so the user cannot back out;
 * the caller hides it. Shown and hidden from any task (the desktop applies the change). */
#include <string.h>

#include "desktop_int.h"
#include "pal/dialog.h"
#include "pal/overlay.h"
#include "pal/shell.h"
#include "pal/theme.h"
#include "theme_widgets.h"

#define BOX_W 200
#define BOX_H 56
#define TICK_MS 100

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full);
static bool key(pal_view_t *v, const pal_key_t *k);
static void tick(pal_view_t *v, uint32_t now);
static void leave(pal_view_t *v);
static bool describe(pal_view_t *v, pal_shell_view_t *o);

static struct {
    pal_view_t view;
    bool pushed;
    char text[PAL_DIALOG_TEXT];
    int frame;
} BY = {.view = {.id = "busy", .title = "Busy", .draw = draw, .key = key, .leave = leave, .tick = tick,
                 .tick_ms = TICK_MS, .data = &BY, .describe = describe}};

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    const pal_font_t *f = t->tokens.caption;
    int w = BOX_W < a.w - 16 ? BOX_W : a.w - 16;
    pal_rect_t box = {a.x + (a.w - w) / 2, a.y + (a.h - BOX_H) / 2, w, BOX_H};
    pal_def_frame(c, t, box, NULL);
    t->comp.spinner(c, t, box.x + box.w / 2, box.y + 18, BY.frame);
    int cols = (w - 8) / f->w;
    char line[PAL_DIALOG_TEXT];
    nema_strlcpy(line, BY.text, sizeof(line));
    if (cols < (int)strlen(line)) line[cols < 0 ? 0 : cols] = 0;
    int tw = pal_text_w(f, line);
    pal_text(c, f, box.x + (box.w - tw) / 2, box.y + 34, line, PAL_C_INK, PAL_TRANSPARENT);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v, (void)k;
    return true;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    BY.frame++;
    pal_desktop_invalidate();
}

static bool describe(pal_view_t *v, pal_shell_view_t *o) {
    (void)v;
    o->kind = PAL_SHELL_BUSY;
    o->text = BY.text;
    o->sel = BY.frame;
    return true;
}

static void leave(pal_view_t *v) {
    (void)v;
    BY.pushed = false;
}

static void post_show(void *arg) {
    (void)arg;
    if (BY.pushed) {
        pal_desktop_invalidate();
        return;
    }
    if (ovl_push_now(&BY.view, 0) == NEMA_OK) BY.pushed = true;
}

static void post_hide(void *arg) {
    (void)arg;
    if (BY.pushed) ovl_remove_now(&BY.view);
}

void pal_busy_show(const char *text) {
    desk_lock();
    nema_strlcpy(BY.text, text ? text : "", sizeof(BY.text));
    desk_unlock();
    if (!desk_started() || desk_on_task()) post_show(NULL);
    else desk_post(post_show, NULL);
}

void pal_busy_hide(void) {
    if (!desk_started() || desk_on_task()) post_hide(NULL);
    else desk_post(post_hide, NULL);
}

bool pal_busy_showing(void) { return BY.pushed; }
