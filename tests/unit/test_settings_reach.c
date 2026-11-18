/* ADR-0010 §7: every system setting is reachable in the Settings of every session, and a session's own settings only under that session.
 * Walks the Settings app's pages (apps/settings*.c) the way a person would, from the root through every submenu that is not left out,
 * and collects the key behind each row (its .key, or the key its generic callbacks read). The sessions here are stand-ins with the real
 * ids: what decides a row is the running session's id. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/nema.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/display.h"
#include "pal/menu.h"
#include "pal/settings_schema.h"
#include "pal/theme.h"
#include "settings_int.h"
#include "test.h"

extern pal_menu_page_t PG_ROOT;

static bool start_ok(void) { return true; }
static const pal_desktop_env_t CLASSIC = {.id = "classic", .title = "Classic", .start = start_ok};
static const pal_desktop_env_t ATHENA = {.id = "athena", .title = "Athena", .start = start_ok};

#define MAX_KEYS 128
#define MAX_PAGES 96
static const char *s_keys[MAX_KEYS];
static int s_nkeys;
static pal_menu_page_t *s_seen[MAX_PAGES];
static int s_nseen;

static const char *row_key(const pal_menu_row_t *r) {
    if (r->key) return r->key;
    if (r->get == settings_bool_get || r->get == settings_int_get) return (const char *)r->ctx;
    if (r->get == settings_map_get) return ((const settings_map_t *)r->ctx)->key;
    if (r->text == settings_text_get) return (const char *)r->ctx;
    return NULL;
}

static void walk(pal_menu_page_t *p) {
    if (!p) return;
    for (int i = 0; i < s_nseen; i++)
        if (s_seen[i] == p) return;
    if (s_nseen == MAX_PAGES) return;
    s_seen[s_nseen++] = p;
    if (getenv("REACH_TRACE")) fprintf(stderr, "page %s\n", p->title ? p->title : "(root)");
    if (p->build) p->build(p);
    for (size_t i = 0; i < p->n; i++) {
        const pal_menu_row_t *r = &p->rows[i];
        if (pal_menu_row_hidden(r)) continue;
        const char *k = row_key(r);
        if (k && s_nkeys < MAX_KEYS) s_keys[s_nkeys++] = k;
        if (r->kind == PAL_MENU_SUBMENU && r->open) walk(r->open(r->ctx));
    }
}

static bool reached(const char *key) {
    for (int i = 0; i < s_nkeys; i++)
        if (!strcmp(s_keys[i], key)) return true;
    return false;
}

static void check_session(const char *id) {
    CHECK(pal_desktops_use(id) == NEMA_OK);
    s_nkeys = s_nseen = 0;
    walk(&PG_ROOT);
    for (size_t i = 0; i < pal_settings_schema_count(); i++) {
        const pal_setting_desc_t *d = pal_settings_schema_get(i);
        if (d->internal || (d->caps && !pal_has(d->caps))) continue; /* the stand-in board has no radios */
        bool want = !d->owner || !strcmp(d->owner, id);
        g_checks++;
        if (want && !reached(d->key)) g_failures++, printf("  FAIL %s: %s is not reachable in Settings\n", id, d->key);
        if (d->owner && strcmp(d->owner, id) && reached(d->key)) g_failures++, printf("  FAIL %s: %s belongs to %s but shows\n", id, d->key, d->owner);
    }
}

TEST(every_system_setting_is_reachable_under_classic) { check_session("classic"); }
TEST(every_system_setting_is_reachable_under_athena) { check_session("athena"); }

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    pal_theme_select(0, false, 0);
    pal_desktop_cfg_t dc = {.status = NULL};
    pal_desktop_init(&dc); /* the compositor the sessions switch on (no task runs) */
    pal_desktops_register(&CLASSIC);
    pal_desktops_register(&ATHENA);
    RUN(every_system_setting_is_reachable_under_classic);
    RUN(every_system_setting_is_reachable_under_athena);
    return TEST_DONE();
}
