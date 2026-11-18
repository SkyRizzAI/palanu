#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/desktop.h"
#include "pal/desktops.h"
#include "pal/kui.h"
#include "pal/registry.h"
#include "pal/theme.h"
#include "pal/ui.h"
#include "ui_launcher.h"

/* The home app for apps: the registry's launchable entries drawn in one of the four reference skins (ui.launcher: strip,
 * grid, compact, list; ui_launcher.c). It pins Terminal, Apps, Files, Settings and Logs (PINNED_IDS); the other apps are in the
 * app list (applist.c). Hidden entries and services are not listed. Enter on a built-in view opens it in the desktop; Enter on an
 * installed tty app opens the Terminal and types its command there (pal_launcher_set_runner). An installed gui app has no
 * frame API yet, so it is listed and not started. Left and Up go back one entry, Right and Down forward one, in every
 * skin (the reference Prev and Next). TAB opens the app's information page; Esc goes back to Home. */

static struct {
    int sel;
    bool info;
    bool fixture; /* uishow launcher SKIN ref: the reference entries (launcher_fixture.c) */
    void (*run_tty)(const char *id);
} L;

static pal_reg_app_t ENT[UI_LAUNCHER_MAX] NEMA_EXT_BSS;
static int NENT;
static kui_canvas_t KC NEMA_EXT_BSS; /* the logical canvas the skin draws into */

/* The pinned entries, in this order: Terminal (not in the reference; the golden tool's start opens it as the first entry), the Apps
 * entry that opens the app list (the first reference entry), then the reference order for Files, Settings and Logs. Every other launchable
 * app is listed in the app list instead, as the reference does. */
static const char *const PINNED_IDS[] = {"terminal", "applist", "files", "settings", "logs", NULL};

/* legends for the legend bar: {key, label} per action. The bar is a system bar drawn under the view, so this view draws no
 * hint line; the table is read by the bar's wiring. */
const char *const PAL_LAUNCHER_LEGENDS[][2] = {{"<>", "move"}, {"ENTER", "launch"}, {"TAB", "info"}};

bool pal_launcher_pinned(const char *id) {
    for (int i = 0; PINNED_IDS[i]; i++)
        if (!strcmp(PINNED_IDS[i], id)) return true;
    return false;
}

/* Listed: visible, something to run, and part of the running session (the manifest's desktops). */
static bool launchable(const pal_reg_app_t *e) {
    return !e->hidden && (e->kind == PAL_KIND_TTY || e->kind == PAL_KIND_GUI) && psys_reg_in_desktop(e, pal_desktops_active());
}

size_t psys_launcher_count(void) {
    size_t n = 0, total = psys_reg_count();
    pal_reg_app_t e;
    for (size_t i = 0; i < total; i++)
        if (psys_reg_get(i, &e) && launchable(&e)) n++;
    return n;
}

bool psys_launcher_get(size_t idx, pal_reg_app_t *out) {
    size_t n = 0, total = psys_reg_count();
    for (size_t i = 0; i < total; i++) {
        if (!psys_reg_get(i, out) || !launchable(out)) continue;
        if (n++ == idx) return true;
    }
    return false;
}

void pal_launcher_set_runner(void (*run_tty)(const char *id)) { L.run_tty = run_tty; }

void pal_ui_launcher_fixture(bool on) { L.fixture = on; }

static bool listed(const char *id) {
    for (int i = 0; i < NENT; i++)
        if (!strcmp(ENT[i].id, id)) return true;
    return false;
}

static void snapshot(void) {
    NENT = 0;
    if (L.fixture) {
        NENT = ui_fixture_entries(ENT, UI_LAUNCHER_MAX);
    } else {
        pal_reg_app_t e;
        for (int f = 0; PINNED_IDS[f] && NENT < UI_LAUNCHER_MAX; f++)
            if (psys_reg_find(PINNED_IDS[f], &e) && launchable(&e) && !listed(e.id)) ENT[NENT++] = e;
    }
    if (L.sel >= NENT) L.sel = NENT ? NENT - 1 : 0;
    if (L.sel < 0) L.sel = 0;
}

static const char *kind_name(pal_kind_t k) {
    switch (k) {
    case PAL_KIND_GUI: return "gui";
    case PAL_KIND_TTY: return "tty";
    default: return "service";
    }
}

static void caps_text(uint32_t caps, char *out, size_t cap) {
    static const struct {
        uint32_t bit;
        const char *name;
    } NAMES[] = {{PAL_PERM_FS, "fs"}, {PAL_PERM_NET, "net"}, {PAL_PERM_FS_HOME, "fs.home"}, {PAL_PERM_FS_MEDIA, "fs.media"},
                 {PAL_PERM_NET_LISTEN, "net.listen"}, {PAL_PERM_PROC, "proc"}, {PAL_PERM_CRYPTO_KEYS, "crypto.keys"},
                 {PAL_PERM_SETTINGS_READ, "settings.read"}, {PAL_PERM_SETTINGS_WRITE, "settings.write"},
                 {PAL_PERM_NOTIFY, "notify"}, {PAL_PERM_SVC_CONTROL, "svc.control"}, {PAL_PERM_LINK, "link"}, {PAL_PERM_DEV, "dev"}};
    out[0] = 0;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!(caps & NAMES[i].bit)) continue;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", n ? ", " : "", NAMES[i].name);
    }
    if (!out[0]) snprintf(out, cap, "none");
}

static void info_page(pal_canvas_t *c, const pal_theme_t *t, pal_rect_t a, const pal_reg_app_t *e) {
    int m = 8;
    pal_ui_box(c, a.x + m, a.y + m, a.w - 2 * m, a.h - 2 * m, 4, PAL_C_PAPER, PAL_C_INK);
    int x = a.x + m + 8, y = a.y + m + 6;
    char line[96], caps[80];
    pal_text(c, t->tokens.title, x, y, e->name[0] ? e->name : e->id, PAL_C_INK, PAL_TRANSPARENT);
    y += t->tokens.title->h + 6;
    caps_text(e->caps, caps, sizeof(caps));
    char mem[24];
    if (e->mem_kb) snprintf(mem, sizeof(mem), "%u KB", (unsigned)e->mem_kb);
    else snprintf(mem, sizeof(mem), "not limited");
    const char *src = e->source == PAL_SRC_BUILTIN ? "built-in" : "installed";
    const struct {
        const char *k, *v;
    } rows[] = {{"id", e->id}, {"version", e->version[0] ? e->version : "-"}, {"kind", kind_name(e->kind)},
                {"source", src}, {"memory", mem}, {"caps", caps}};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        snprintf(line, sizeof(line), "%-8s %s", rows[i].k, rows[i].v);
        pal_text(c, t->tokens.body, x, y, line, PAL_C_INK, PAL_TRANSPARENT);
        y += t->tokens.body->h + 2;
    }
}

/* The info page's only legend; the skin has none (the reference launcher draws no legends). */
static const pal_legend_item_t INFO_LEGENDS[] = {{PAL_ACT_BACK, "back", NULL}};

/* The skin fills the whole area under the status bar: the reference launcher draws no footer. */
static void draw(pal_view_t *self, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)self, (void)full;
    const pal_theme_t *t = pal_theme_active();
    pal_ui_prefs_load();
    snapshot();
    kui_canvas_init(&KC, c->w, c->h, kui_scale_q());
    kui_clip(&KC, 0, kui_lg(&KC, a.y), KC.w, kui_lg(&KC, a.y + a.h) - kui_lg(&KC, a.y)); /* the area under the status bar; the bar keeps its pixels */
    ui_entries_t E = {ENT, NENT, L.sel, L.fixture ? "PALANU" : pal_ui_device_name()};
    ui_skin_draw(&KC, pal_ui_launcher(), &E);
    kui_present(c, &KC, a);
    if (L.info && NENT) {
        pal_legend_set(INFO_LEGENDS, 1);
        info_page(c, t, a, &ENT[L.sel]);
    } else {
        pal_legend_clear();
    }
}

/* Opens an entry: a built-in view in the desktop, an installed tty app in the Terminal (the app list uses this too). */
void pal_launcher_run(const pal_reg_app_t *e) {
    if (e->kind == PAL_KIND_GUI && e->source == PAL_SRC_BUILTIN) pal_desktop_open(e->id);
    else if (e->kind == PAL_KIND_TTY && L.run_tty) L.run_tty(e->id);
}

static void activate(void) {
    if (L.sel < 0 || L.sel >= NENT) return;
    pal_launcher_run(&ENT[L.sel]);
}

static bool key(pal_view_t *self, const pal_key_t *k) {
    (void)self;
    snapshot();
    int n = NENT;
    if (L.info) {
        if (k->sym == PAL_KEY_ESC || k->sym == PAL_KEY_TAB || k->sym == PAL_KEY_ENTER) L.info = false;
        return true;
    }
    if (!n) {
        if (k->sym == PAL_KEY_ESC) pal_desktop_home();
        return k->sym == PAL_KEY_ESC;
    }
    if (k->sym == PAL_KEY_LEFT || k->sym == PAL_KEY_UP) L.sel = L.sel > 0 ? L.sel - 1 : 0;
    else if (k->sym == PAL_KEY_RIGHT || k->sym == PAL_KEY_DOWN) L.sel = L.sel + 1 < n ? L.sel + 1 : n - 1;
    else if (k->sym == PAL_KEY_HOME) L.sel = 0;
    else if (k->sym == PAL_KEY_END) L.sel = n - 1;
    else if (k->sym == PAL_KEY_ENTER) activate();
    else if (k->sym == PAL_KEY_TAB) L.info = true;
    else if (k->sym == PAL_KEY_ESC) pal_desktop_home();
    else if (k->sym == PAL_KEY_CHAR && !(k->mods & ~PAL_MOD_SHIFT)) { /* jump to the next app starting with that letter */
        for (int s = 1; s <= n; s++) {
            int j = (L.sel + s) % n;
            if (ENT[j].name[0] && tolower((unsigned char)ENT[j].name[0]) == tolower((int)k->cp)) {
                L.sel = j;
                break;
            }
        }
    } else return false;
    return true;
}

pal_view_t pal_launcher_view = {.id = "launcher", .title = "Apps", .flags = PAL_VIEW_HIDDEN, .draw = draw, .key = key};
