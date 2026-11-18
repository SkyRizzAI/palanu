/* Menu rows made from the settings schema (pal/settings_schema.h): what a session's Settings shows for keys it has no page of its own
 * for. The row's ctx is its descriptor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/types.h"
#include "pal/desktops.h"
#include "pal/menu.h"
#include "pal/settings.h"
#include "pal/settings_schema.h"

#define SESSIONS_MAX PAL_DESKTOP_ENV_MAX

static const char *s_sess_ids[SESSIONS_MAX + 1];
static const char *s_sess_titles[SESSIONS_MAX + 1];

static void sessions_fill(void) {
    size_t n = 0;
    for (; n < pal_desktops_count() && n < SESSIONS_MAX; n++) {
        s_sess_ids[n] = pal_desktops_get(n)->id;
        s_sess_titles[n] = pal_desktops_get(n)->title;
    }
    s_sess_ids[n] = s_sess_titles[n] = NULL;
}

static const char *const *names_of(const pal_setting_desc_t *d) { return d->dynamic ? s_sess_ids : d->names; }

static int text_get(const pal_setting_desc_t *d, char *out, size_t cap) {
    out[0] = 0;
    return psys_settings_get(d->key, out, cap);
}

static int bool_get(void *ctx) {
    char v[8];
    text_get(ctx, v, sizeof(v));
    return v[0] == '1' || !strcmp(v, "on") || !strcmp(v, "true");
}
static void bool_set(void *ctx, int v) { (void)psys_settings_set(((const pal_setting_desc_t *)ctx)->key, v ? "1" : "0"); }

static int int_get(void *ctx) {
    char v[16];
    text_get(ctx, v, sizeof(v));
    return atoi(v);
}
static void int_set(void *ctx, int v) {
    char t[16];
    snprintf(t, sizeof(t), "%d", v);
    (void)psys_settings_set(((const pal_setting_desc_t *)ctx)->key, t);
}

static int choice_get(void *ctx) {
    const pal_setting_desc_t *d = ctx;
    char v[32];
    if (d->dynamic) nema_strlcpy(v, pal_desktops_active(), sizeof(v));
    else text_get(d, v, sizeof(v));
    const char *const *names = names_of(d);
    for (int i = 0; names && names[i]; i++)
        if (!strcmp(names[i], v)) return i;
    return 0;
}
static void choice_set(void *ctx, int i) {
    const pal_setting_desc_t *d = ctx;
    const char *const *names = names_of(d);
    if (!names || !names[i]) return;
    if (d->dynamic) (void)pal_desktops_use(names[i]); /* desktop.env: switch the session now; the registry keeps the choice */
    else (void)psys_settings_set(d->key, names[i]);
}

static void text_text(void *ctx, char *out, size_t cap) { text_get(ctx, out, cap); }
static bool text_apply(void *ctx, const char *t) { return psys_settings_set(((const pal_setting_desc_t *)ctx)->key, t) == 0; }
static void pin_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    char v[8];
    psys_settings_get("security.pin", v, sizeof(v));
    nema_strlcpy(out, !strcmp(v, "set") ? "Set" : "None", cap);
}

void pal_settings_schema_rows(pal_menu_page_t *p, const char *owner, const char *group) {
    sessions_fill();
    for (size_t i = 0; i < pal_settings_schema_count(); i++) {
        const pal_setting_desc_t *d = pal_settings_schema_get(i);
        if (d->internal || (owner ? !d->owner || strcmp(d->owner, owner) : d->owner != NULL)) continue;
        if (group && strcmp(d->group, group)) continue;
        if (d->kind == PAL_SETTING_CHOICE && d->dynamic && strcmp(d->key, "desktop.env")) continue;
        pal_menu_row_t r = {.label = d->label, .hint = d->help, .ctx = (void *)d, .key = d->key};
        switch (d->kind) {
        case PAL_SETTING_BOOL: r.kind = PAL_MENU_TOGGLE, r.get = bool_get, r.set = bool_set; break;
        case PAL_SETTING_INT:
            r.kind = PAL_MENU_SLIDER, r.get = int_get, r.set = int_set, r.min = d->min, r.max = d->max, r.step = d->step ? d->step : 1;
            break;
        case PAL_SETTING_CHOICE:
            r.kind = PAL_MENU_CHOICE, r.get = choice_get, r.set = choice_set;
            r.choices = d->dynamic ? s_sess_titles : d->labels ? d->labels : d->names;
            break;
        case PAL_SETTING_TEXT: r.kind = PAL_MENU_TEXT, r.text = text_text, r.apply_text = text_apply, r.max_len = d->max; break;
        case PAL_SETTING_PIN: r.kind = PAL_MENU_TEXT, r.text = pin_text, r.apply_text = text_apply, r.mask = true, r.max_len = 8; break;
        }
        pal_menu_add(p, &r);
    }
}
