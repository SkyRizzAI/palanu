/* Default components for the dialogs, text fields, toasts and spinners. Themes inherit them through
 * pal_components_fill(); see pal/theme.h. Internal to the system. */
#ifndef SYSTEM_THEME_WIDGETS_H
#define SYSTEM_THEME_WIDGETS_H

#include "pal/theme.h"

pal_rect_t pal_def_dialog_box(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t area, const char *title, const char *text,
                              const char *const *buttons, int nbuttons, int selected);
int pal_def_text_field(pal_canvas_t *c, const pal_theme_t *t, int x, int y, int w, const char *shown, int caret,
                       bool caret_on);
void pal_def_toast(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t box, const char *text);
void pal_def_spinner(pal_canvas_t *c, const pal_theme_t *t, int cx, int cy, int frame);

/* Frame of a popup in box: fill, one-pixel rounded outline, and an inverted title strip when title is not empty. Returns the
 * height the strip takes (0 without a title), so the caller can place its content below. */
int pal_def_frame(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t box, const char *title);

/* Copies the default for every NULL member of comp (the theme's own members stay). */
void pal_components_fill(pal_components_t *comp);

#endif
