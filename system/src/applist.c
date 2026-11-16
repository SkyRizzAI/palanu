/* The app list (the reference AppListScreen): every launchable app the launcher does not pin, opened from the launcher's Apps entry.
 * Apps group into folders by category, as the reference Launchpad does (an empty, "Apps" or "System" category stays at the top):
 * the root lists the folders (alphabetically), then the top-level apps (alphabetically); a folder opens with a Back row.
 * Enter launches an app (launcher.c's rule: built-in views open in the desktop, installed Terminal apps run in the Terminal);
 * Esc goes up a folder, then back to the launcher. Drawing is kui_applist (kui_applist.c). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/desktop.h"
#include "pal/icons.h"
#include "pal/kui.h"
#include "pal/registry.h"
#include "pal/ui.h"
#include "ui_launcher.h"

#define ROWS_MAX 32

static const uint8_t K_FOLDER[8] = {0x00, 0x70, 0xFF, 0x81, 0x81, 0x81, 0xFF, 0x00}; /* The reference file.folder */
static const uint8_t K_NAVUP[6] = {0x00, 0x00, 0x30, 0x78, 0xCC, 0x00};              /* The reference nav.up, 6x6 */
static const pal_icon_t ICON_FOLDER = {"file.folder", 8, 8, 8, K_FOLDER, NULL};
static const pal_icon_t ICON_NAVUP = {"nav.up", 8, 6, 6, K_NAVUP, NULL};

static struct {
    bool fixture;
    char folder[16];   /* the open folder; empty at the root */
    int cursor, first;
} A;

typedef struct {
    char label[32];
    char folder[16]; /* the folder an app belongs to; a folder row's own name */
    bool is_folder, is_back;
    const pal_icon_t *icon;
    pal_reg_app_t reg;
} row_t;

static row_t R[ROWS_MAX] NEMA_EXT_BSS;
static kui_row_t KR[ROWS_MAX] NEMA_EXT_BSS;
static int NR;
static kui_canvas_t KC NEMA_EXT_BSS;

void pal_ui_applist_fixture(bool on) { A.fixture = on; }

static bool listable(const pal_reg_app_t *e) {
    return !e->hidden && (e->kind == PAL_KIND_TTY || e->kind == PAL_KIND_GUI) && !pal_launcher_pinned(e->id);
}

/* The folder of an app: its category, unless that is empty, "Apps" or "System" (then the root). */
static void folder_of(const pal_reg_app_t *e, char *out, size_t cap) {
    out[0] = 0;
    if (!e->category[0] || !strcmp(e->category, "Apps") || !strcmp(e->category, "System")) return;
    snprintf(out, cap, "%s", e->category);
}

static bool name_less(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca < cb;
        if (!ca) return false;
    }
}

static void sort_rows(int from, int to) {
    for (int i = from + 1; i < to; i++)
        for (int j = i; j > from && name_less(R[j].label, R[j - 1].label); j--) {
            row_t t = R[j];
            R[j] = R[j - 1];
            R[j - 1] = t;
        }
}

static const pal_icon_t *icon_for(const char *name) {
    const pal_icon_t *ic = ui_fixture_icon(name);
    if (!ic) ic = pal_icon_find(name, 16);
    if (!ic) ic = pal_icon_find("default", 16);
    return ic;
}

static void add_row(const char *label, const char *folder, bool is_folder, bool is_back, const pal_icon_t *ic,
                    const pal_reg_app_t *reg) {
    if (NR >= ROWS_MAX) return;
    row_t *r = &R[NR++];
    memset(r, 0, sizeof(*r));
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->folder, sizeof(r->folder), "%s", folder);
    r->is_folder = is_folder;
    r->is_back = is_back;
    r->icon = ic;
    if (reg) r->reg = *reg;
}

static void build(void) {
    NR = 0;
    pal_reg_app_t e;
    if (A.fixture) {
        /* static: 32 registry entries are ~6 KB, as big as the desktop task's whole stack (it overflowed on the board) */
        static pal_reg_app_t apps[ROWS_MAX] NEMA_EXT_BSS;
        int n = ui_fixture_apps(apps, ROWS_MAX);
        for (int i = 0; i < n; i++) add_row(apps[i].name, "", false, false, icon_for(ui_entry_icon(&apps[i])), &apps[i]);
        return;
    }
    if (A.folder[0]) {
        add_row("Back", "", false, true, &ICON_NAVUP, NULL);
        int from = NR;
        size_t total = psys_reg_count();
        for (size_t i = 0; i < total; i++) {
            if (!psys_reg_get(i, &e) || !listable(&e)) continue;
            char f[16];
            folder_of(&e, f, sizeof(f));
            if (strcmp(f, A.folder)) continue;
            add_row(e.name[0] ? e.name : e.id, "", false, false, icon_for(ui_entry_icon(&e)), &e);
        }
        sort_rows(from, NR);
        return;
    }
    /* root: folders first (distinct, alphabetical), then the top-level apps (alphabetical) */
    size_t total = psys_reg_count();
    for (size_t i = 0; i < total; i++) {
        if (!psys_reg_get(i, &e) || !listable(&e)) continue;
        char f[16];
        folder_of(&e, f, sizeof(f));
        if (!f[0]) continue;
        bool seen = false;
        for (int k = 0; k < NR; k++)
            if (R[k].is_folder && !strcmp(R[k].label, f)) seen = true;
        if (!seen) add_row(f, f, true, false, &ICON_FOLDER, NULL);
    }
    sort_rows(0, NR);
    int from = NR;
    for (size_t i = 0; i < total; i++) {
        if (!psys_reg_get(i, &e) || !listable(&e)) continue;
        char f[16];
        folder_of(&e, f, sizeof(f));
        if (f[0]) continue;
        add_row(e.name[0] ? e.name : e.id, "", false, false, icon_for(ui_entry_icon(&e)), &e);
    }
    sort_rows(from, NR);
}

static void enter_folder(const char *folder) {
    snprintf(A.folder, sizeof(A.folder), "%s", folder);
    A.cursor = 0;
    A.first = 0;
}

static void activate(void) {
    build();
    if (A.cursor < 0 || A.cursor >= NR) return;
    row_t *r = &R[A.cursor];
    if (r->is_back) enter_folder("");
    else if (r->is_folder) enter_folder(r->label);
    else if (r->reg.id[0]) pal_launcher_run(&r->reg);
}

static void draw(pal_view_t *self, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)self, (void)full;
    pal_ui_prefs_load();
    build();
    kui_canvas_init(&KC, c->w, c->h, kui_scale_q());
    kui_clip(&KC, 0, kui_lg(&KC, a.y), KC.w, kui_lg(&KC, a.y + a.h) - kui_lg(&KC, a.y));
    for (int i = 0; i < NR; i++) {
        KR[i].label = R[i].label;
        KR[i].bits = R[i].icon ? R[i].icon->bits : NULL;
        KR[i].w = R[i].icon ? R[i].icon->w : 0;
        KR[i].h = R[i].icon ? R[i].icon->h : 0;
        KR[i].chevron = !R[i].is_back;
    }
    if (NR == 0) {
        const char *msg = "No apps installed";
        kui_text(&KC, &FONT_REG8, (KC.w - kui_text_w(&FONT_REG8, msg)) / 2, KC.clip_y0 + 4, msg, true);
    } else {
        kui_applist(&KC, KR, NR, A.cursor, &A.first);
    }
    kui_present(c, &KC, a);
}

static bool key(pal_view_t *self, const pal_key_t *k) {
    (void)self;
    build();
    int n = NR;
    if (k->sym == PAL_KEY_ESC) {
        if (A.folder[0]) enter_folder("");
        else pal_desktop_open("launcher");
        return true;
    }
    if (!n) return k->sym == PAL_KEY_ENTER;
    if (k->sym == PAL_KEY_LEFT || k->sym == PAL_KEY_UP) A.cursor = A.cursor > 0 ? A.cursor - 1 : 0;
    else if (k->sym == PAL_KEY_RIGHT || k->sym == PAL_KEY_DOWN) A.cursor = A.cursor + 1 < n ? A.cursor + 1 : n - 1;
    else if (k->sym == PAL_KEY_HOME) A.cursor = 0;
    else if (k->sym == PAL_KEY_END) A.cursor = n - 1;
    else if (k->sym == PAL_KEY_ENTER) activate();
    else return false;
    return true;
}

pal_view_t pal_applist_view = {.id = "applist", .title = "Apps", .flags = PAL_VIEW_HIDDEN, .draw = draw, .key = key};
