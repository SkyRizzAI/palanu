/* The Files app (view "files"): a browser over the namespace (/home, /data, /apps, /tmp, /sd), with the same operations as the reference's
 * file browser: open, view (the text viewer), copy, cut, paste, rename, delete, new folder, new text file, info, and install of a
 * .papp package. Listing and operations run in slices from the pump, so a big directory or copy never stalls the keys. Model:
 * files_model.c; operations: files_ops.c; the viewer: textview.c. Policy here: installed apps (/apps) are removed from Settings,
 * mount points are not removed, and deleting under /data asks twice (the text says so). The listing is also bound as a form
 * (pal_menu_bind, ADR-0012): the classic session draws it with draw below, a session that draws forms (Athena) in its own look. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "builtin.h"
#include "files_model.h"
#include "files_menu.h"
#include "files_ops.h"
#include "libapp.h"
#include "pal/dialog.h"
#include "pal/desktop.h"
#include "pal/kui.h"
#include "pal/menu.h"
#include "pal/nav.h"
#include "pal/overlay.h"
#include "pal/papp.h"
#include "pal/sysinfo.h"
#include "pal/time.h"
#include "pal/toast.h"
#include "pal/ui.h"
#include "pal/vk.h"
#include "views.h"

#define LIST_PER_TICK 48   /* entries read per pump call */
#define BUSY_MS 200        /* busy text refresh while a job runs */
#define MAX_MENU 16 /* the ops menu rows (files_menu.h holds the same count) */

typedef enum {
    A_NONE, A_VIEW, A_OPEN, A_COPY, A_CUT, A_PASTE, A_RENAME, A_DELETE, A_INFO, A_NEWDIR, A_NEWFILE, A_INSTALL,
} act_t;

typedef struct {
    act_t act;
    char path[NEMA_PATH_MAX];  /* the entry (or the directory for paste and new items) */
    char dst[NEMA_PATH_MAX];   /* a pending paste or rename target */
    bool move;
    bool is_dir;
} target_t;

static struct {
    files_list_t L;
    char cwd[NEMA_PATH_MAX];
    char start[NEMA_PATH_MAX];  /* the directory Files opens at (pal_files_open) */
    bool want;                  /* a pal_files_open is waiting for the pump */
    char came[NEMA_NAME_MAX];   /* the folder we came up from: selected after the listing */
    int sel, top, vis;
    char clip[NEMA_PATH_MAX];
    bool clip_has, clip_move;
    files_job_t *job;
    uint32_t busy_ms;
    bool busy_on;
    int defer;                  /* pump ticks before a deferred action (install) runs */
    act_t defer_act;
    target_t tgt;               /* the entry the menu or a dialog is about */
    pal_vk_entry_t ve;
    char vebuf[NEMA_NAME_MAX];
    act_t ve_act;
    char menu_text[MAX_MENU][24];
    act_t menu_act[MAX_MENU];
    const char *menu_items[MAX_MENU];
    int menu_n;
} F NEMA_EXT_BSS;

static void go(const char *path);
static void reload(void);
static void perform(act_t act);

/* ---- rows ---- */
static bool at_roots(void) { return F.L.roots; }
static bool has_up(void) { return !F.L.roots; }

static int rows_total(void) { return F.L.n + (has_up() ? 1 : 0); }

/* The entry of a row: NULL for "..". */
static const files_ent_t *row_ent(int row) {
    if (has_up()) row--;
    return row >= 0 && row < F.L.n ? &F.L.ents[row] : NULL;
}

static void row_path(int row, char *out, size_t cap) {
    const files_ent_t *e = row_ent(row);
    if (!e) files_parent(out, cap, F.cwd);
    else files_join(out, cap, F.cwd, e->name);
}

/* Keeps the cursor on a row, and the rows it is shown with, when the list changes. */
static void clamp_sel(void) {
    int n = rows_total();
    if (F.sel >= n) F.sel = n - 1;
    if (F.sel < 0) F.sel = 0;
    if (F.sel < F.top) F.top = F.sel;
    if (F.vis > 0 && F.sel >= F.top + F.vis) F.top = F.sel - F.vis + 1;
    if (F.top < 0) F.top = 0;
}

/* ---- navigation ---- */
static void go(const char *path) {
    if (files_list_begin(&F.L, path) != NEMA_OK) {
        psys_toast("Out of memory", 1500);
        return;
    }
    snprintf(F.cwd, sizeof(F.cwd), "%s", path);
    F.sel = 0;
    F.top = 0;
    pal_desktop_invalidate();
}

static void on_loaded(void) {
    if (F.L.err != NEMA_OK) psys_toast("Cannot read folder", 1500);
    if (F.came[0]) {
        int i = files_find(&F.L, F.came);
        if (i >= 0) F.sel = i + (has_up() ? 1 : 0);
        F.came[0] = 0;
    }
    clamp_sel();
}

static void go_up(void) {
    char parent[NEMA_PATH_MAX];
    if (!files_parent(parent, sizeof(parent), F.cwd)) return;
    snprintf(F.came, sizeof(F.came), "%s", files_base(F.cwd));
    go(parent);
}

/* Reloads the folder and keeps the cursor on the same name when it still exists. */
static void reload(void) {
    char name[NEMA_NAME_MAX] = "";
    const files_ent_t *e = row_ent(F.sel);
    if (e) snprintf(name, sizeof(name), "%s", e->name);
    char dir[NEMA_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", F.cwd);
    go(dir);
    snprintf(F.came, sizeof(F.came), "%s", name);
}

static void activate(void) {
    const files_ent_t *e = row_ent(F.sel);
    if (!e) {
        go_up();
        return;
    }
    char path[NEMA_PATH_MAX];
    row_path(F.sel, path, sizeof(path));
    if (e->type == FILES_T_ROOT || e->type == NEMA_T_DIR) {
        go(path);
        return;
    }
    if (files_kind(e) == FILES_K_PACKAGE) {
        snprintf(F.tgt.path, sizeof(F.tgt.path), "%s", path);
        F.tgt.act = A_INSTALL;
        perform(A_INSTALL);
        return;
    }
    pal_textview_open(path);
}

/* ---- jobs ---- */
static void nop(bool ok, void *ctx) {
    (void)ok, (void)ctx;
}

static void job_finished(void) {
    pal_busy_hide();
    F.busy_on = false;
    files_op_t kind = files_job_kind(F.job);
    nema_err_t r = files_job_result(F.job);
    if (r != NEMA_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%s failed: %s", kind == FILES_OP_DELETE ? "Delete" : kind == FILES_OP_MOVE ? "Move" : "Copy",
                 pal_strerror(r));
        psys_toast(msg, 3000);
    } else {
        psys_toast(kind == FILES_OP_DELETE ? "Deleted" : kind == FILES_OP_MOVE ? "Moved" : "Copied", 1500);
    }
    reload();
}

static void start_job(bool copy, const char *src, const char *dst, bool move) {
    if (!F.job) F.job = files_job_new();
    if (!F.job) { psys_toast("Out of memory", 1500); return; }
    nema_err_t r = copy ? files_job_copy(F.job, src, dst, move) : files_job_delete(F.job, src);
    if (r != NEMA_OK) {
        pal_busy_hide();
        F.busy_on = false;
        psys_toast(pal_strerror(r), 2500);
        return;
    }
    if (!files_job_busy(F.job)) return job_finished();
    char text[48];
    files_job_status(F.job, text, sizeof(text));
    pal_busy_show(text);
    F.busy_on = true;
    F.busy_ms = (uint32_t)(pal_sys_uptime_us() / 1000u);
}

static void job_pump(void) {
    if (!F.job || !files_job_busy(F.job)) return;
    if (files_job_step(F.job, 8000)) return job_finished();
    uint32_t now = (uint32_t)(pal_sys_uptime_us() / 1000u);
    if (F.busy_on && now - F.busy_ms >= BUSY_MS) {
        char text[48];
        files_job_status(F.job, text, sizeof(text));
        pal_busy_show(text);
        F.busy_ms = now;
    }
}

/* ---- dialogs and text entry ---- */
static void on_overwrite(bool ok, void *ctx) {
    (void)ctx;
    if (!ok) return;
    if (F.tgt.act == A_PASTE || F.tgt.act == A_RENAME) start_job(true, F.tgt.path, F.tgt.dst, F.tgt.move);
}

static void on_delete(bool ok, void *ctx) {
    (void)ctx;
    if (ok) start_job(false, F.tgt.path, NULL, false);
}

static void on_install(bool ok, void *ctx) {
    (void)ctx;
    if (!ok) return;
    pal_busy_show("Installing...");
    F.busy_on = true;
    F.defer_act = A_INSTALL;
    F.defer = 2; /* the busy overlay is drawn first, then the install runs */
}

static void run_install(void) {
    char id[24] = "", msg[96] = "";
    nema_err_t r = pal_papp_install_msg(F.tgt.path, id, sizeof(id), msg, sizeof(msg));
    pal_busy_hide();
    F.busy_on = false;
    if (r == NEMA_OK) {
        char t[48];
        snprintf(t, sizeof(t), "Installed %s", id);
        psys_toast(t, 2000);
    } else {
        psys_toast(msg[0] ? msg : pal_strerror(r), 3000);
    }
}

static void on_vk(int len, void *ctx) {
    (void)ctx;
    if (len < 0) return;
    if (F.ve_act == A_RENAME) {
        if (!files_valid_name(F.vebuf)) { psys_toast("Invalid name", 1500); return; }
        char parent[NEMA_PATH_MAX];
        files_parent(parent, sizeof(parent), F.tgt.path);
        files_join(F.tgt.dst, sizeof(F.tgt.dst), parent, F.vebuf);
        if (strcmp(F.tgt.dst, F.tgt.path) == 0) return;
        nema_stat_t st;
        if (nema_fs_stat(F.tgt.dst, &st) == NEMA_OK) {
            char body[80];
            snprintf(body, sizeof(body), "Overwrite \"%.40s\"?", F.vebuf);
            F.tgt.act = A_RENAME;
            F.tgt.move = true;
            pal_dialog_confirm("Overwrite", body, "Overwrite", "Cancel", on_overwrite, NULL);
            return;
        }
        F.tgt.move = true;
        start_job(true, F.tgt.path, F.tgt.dst, true);
    } else if (F.ve_act == A_NEWDIR || F.ve_act == A_NEWFILE) {
        if (!files_valid_name(F.vebuf)) { psys_toast("Invalid name", 1500); return; }
        char path[NEMA_PATH_MAX];
        files_join(path, sizeof(path), F.cwd, F.vebuf);
        nema_stat_t st;
        if (nema_fs_stat(path, &st) == NEMA_OK) { psys_toast("Name already exists", 1500); return; }
        nema_err_t r;
        if (F.ve_act == A_NEWDIR) {
            r = nema_fs_mkdir(path);
        } else {
            nema_obj_t *f = NULL;
            r = nema_fs_open(path, NEMA_O_WRITE | NEMA_O_CREATE, &f);
            if (r == NEMA_OK) nema_obj_unref(f);
        }
        if (r != NEMA_OK) { psys_toast(pal_strerror(r), 2000); return; }
        snprintf(F.came, sizeof(F.came), "%s", F.vebuf);
        reload();
    }
}

static void edit_name(act_t act, const char *title, const char *initial) {
    F.ve_act = act;
    snprintf(F.vebuf, sizeof(F.vebuf), "%s", initial ? initial : "");
    memset(&F.ve, 0, sizeof(F.ve));
    F.ve.title = title;
    F.ve.initial = F.vebuf;
    F.ve.buf = F.vebuf;
    F.ve.cap = sizeof(F.vebuf);
    F.ve.allow_empty = false;
    nema_err_t r = pal_vk_edit_async(&F.ve, on_vk, NULL);
    if (r != NEMA_OK) psys_toast(pal_strerror(r), 1500);
}

static void show_info(const char *path, bool is_dir) {
    nema_stat_t st;
    if (nema_fs_stat(path, &st) != NEMA_OK) { psys_toast("Cannot read info", 1500); return; }
    char size[16], shown[40], when[40] = "unknown", hm[8] = "", extra[32] = "";
    files_size_str(size, sizeof(size), st.size);
    files_path_fit(shown, sizeof(shown), path, 34);
    if (st.mtime) {
        pal_tm_t tm;
        pal_time_from_epoch(st.mtime, &tm);
        char d[24];
        pal_ui_fmt_date(&tm, d, sizeof(d));
        pal_ui_fmt_hm(tm.hour, tm.minute, hm, sizeof(hm));
        snprintf(when, sizeof(when), "%s %s", d, hm);
    }
    if (is_dir) {
        bool more = false;
        int n = files_count_tree(path, 2000, &more);
        snprintf(extra, sizeof(extra), "Folder, %d%s items", n - 1, more ? "+" : "");
    } else {
        snprintf(extra, sizeof(extra), "File, %s", size);
    }
    char text[160];
    snprintf(text, sizeof(text), "%s\nModified %s\n%s", extra, when, shown);
    pal_dialog_confirm("Info", text, "Close", NULL, nop, NULL);
}

static bool protected_path(const char *path) { return files_under(path, "/apps") && strcmp(path, "/apps") != 0; }

static bool mount_point(const char *path) {
    static const char *const MOUNTS[] = {"/", "/home", "/data", "/apps", "/tmp", "/sd", "/bin", "/etc"};
    for (size_t i = 0; i < sizeof(MOUNTS) / sizeof(MOUNTS[0]); i++)
        if (strcmp(path, MOUNTS[i]) == 0) return true;
    return false;
}

/* ---- the operations of the menu ---- */
static void do_paste(void) {
    if (!F.clip_has) { psys_toast("Clipboard is empty", 1500); return; }
    nema_stat_t st;
    if (nema_fs_stat(F.clip, &st) != NEMA_OK) {
        F.clip_has = false;
        { psys_toast("Nothing to paste", 1500); return; }
    }
    if (files_under(F.cwd, "/apps")) { psys_toast("Installed apps: use Settings > Apps", 2500); return; }
    files_join(F.tgt.dst, sizeof(F.tgt.dst), F.cwd, files_base(F.clip));
    if (strcmp(F.tgt.dst, F.clip) == 0) { psys_toast("Already here", 1500); return; }
    snprintf(F.tgt.path, sizeof(F.tgt.path), "%s", F.clip);
    F.tgt.move = F.clip_move;
    F.tgt.act = A_PASTE;
    if (F.clip_move) F.clip_has = false; /* a cut is spent once it is pasted; a copy stays on the clipboard */
    if (nema_fs_stat(F.tgt.dst, &st) == NEMA_OK) {
        char body[80];
        snprintf(body, sizeof(body), "Overwrite \"%.40s\"?", files_base(F.clip));
        pal_dialog_confirm("Overwrite", body, "Overwrite", "Cancel", on_overwrite, NULL);
        return;
    }
    start_job(true, F.tgt.path, F.tgt.dst, F.tgt.move);
}

/* The reference delete confirm is a list-style modal: the title 'Delete "name"?' and the rows Delete and
 * Cancel. The picker draws that; its answer is the row index (0 Delete). */
static void on_delete_pick(int index, void *ctx) {
    on_delete(index == 0, ctx);
}

static void do_delete(void) {
    if (protected_path(F.tgt.path)) { psys_toast("Installed app: use Settings > Apps", 2500); return; }
    if (mount_point(F.tgt.path)) { psys_toast("A mount point cannot be removed", 2000); return; }
    static char title[40];
    static const char *const items[2] = {"Delete", "Cancel"};
    snprintf(title, sizeof(title), "Delete \"%.24s\"?", files_base(F.tgt.path));
    pal_picker_open(title, items, 2, on_delete_pick, NULL);
}

static void perform(act_t act) {
    switch (act) {
    case A_VIEW: pal_textview_open(F.tgt.path); break;
    case A_OPEN: go(F.tgt.path); break;
    case A_COPY:
        snprintf(F.clip, sizeof(F.clip), "%s", F.tgt.path);
        F.clip_has = true;
        F.clip_move = false;
        psys_toast("Copied to clipboard", 1200);
        break;
    case A_CUT:
        if (protected_path(F.tgt.path)) { psys_toast("Installed app: use Settings > Apps", 2500); return; }
        if (mount_point(F.tgt.path)) { psys_toast("A mount point cannot be moved", 2000); return; }
        snprintf(F.clip, sizeof(F.clip), "%s", F.tgt.path);
        F.clip_has = true;
        F.clip_move = true;
        psys_toast("Cut to clipboard", 1200);
        break;
    case A_PASTE: do_paste(); break;
    case A_RENAME:
        if (protected_path(F.tgt.path)) { psys_toast("Installed app: use Settings > Apps", 2500); return; }
        if (mount_point(F.tgt.path)) { psys_toast("A mount point cannot be renamed", 2000); return; }
        edit_name(A_RENAME, "Rename", files_base(F.tgt.path));
        break;
    case A_DELETE: do_delete(); break;
    case A_INFO: show_info(F.tgt.path, F.tgt.is_dir); break;
    case A_NEWDIR: edit_name(A_NEWDIR, "New folder", ""); break;
    case A_NEWFILE: edit_name(A_NEWFILE, "New text file", ""); break;
    case A_INSTALL: {
        char name[NEMA_NAME_MAX];
        snprintf(name, sizeof(name), "%s", files_base(F.tgt.path));
        char text[96];
        snprintf(text, sizeof(text), "Install \"%.40s\" as an app?", name);
        pal_dialog_confirm("Install", text, "Install", "Cancel", on_install, NULL);
        break;
    }
    default: break;
    }
}

/* ---- the OPS menu (Tab): the reference file_ops_modal list, titled with the entry's name (files_menu.c) ---- */
static void menu_add(int *n, const char *label, act_t act) {
    if (*n >= MAX_MENU) return;
    snprintf(F.menu_text[*n], sizeof(F.menu_text[*n]), "%s", label);
    F.menu_items[*n] = F.menu_text[*n];
    F.menu_act[*n] = act;
    (*n)++;
}

static void on_menu(int idx, void *ctx) {
    (void)ctx;
    if (idx < 0 || idx >= F.menu_n) return;
    if (F.menu_act[idx] != A_NONE) perform(F.menu_act[idx]); /* A_NONE is Cancel */
}

static void open_menu(void) {
    int n = 0;
    const files_ent_t *e = row_ent(F.sel);
    if (at_roots() && e) {
        snprintf(F.tgt.path, sizeof(F.tgt.path), "%s", e->name);
        F.tgt.is_dir = true;
        menu_add(&n, "Open", A_OPEN);
        menu_add(&n, "Info", A_INFO);
    } else if (!e) {
        /* the ".." row, or the empty listing: the operations are on the folder itself */
        snprintf(F.tgt.path, sizeof(F.tgt.path), "%s", F.cwd);
        F.tgt.is_dir = true;
        if (F.clip_has) menu_add(&n, "Paste", A_PASTE);
        menu_add(&n, "New Folder", A_NEWDIR);
        menu_add(&n, "New text file", A_NEWFILE);
        menu_add(&n, "Info", A_INFO);
    } else {
        row_path(F.sel, F.tgt.path, sizeof(F.tgt.path));
        F.tgt.is_dir = e->type != NEMA_T_FILE;
        if (files_kind(e) == FILES_K_PACKAGE) menu_add(&n, "Install", A_INSTALL);
        if (!F.tgt.is_dir) menu_add(&n, "View", A_VIEW);
        menu_add(&n, "Copy", A_COPY);
        menu_add(&n, "Cut", A_CUT);
        if (F.clip_has) menu_add(&n, "Paste", A_PASTE);
        menu_add(&n, "Rename", A_RENAME);
        menu_add(&n, "Delete", A_DELETE);
        menu_add(&n, "New Folder", A_NEWDIR);
    }
    menu_add(&n, "Cancel", A_NONE);
    F.menu_n = n;
    files_menu_open(F.tgt.path[0] ? files_base(F.tgt.path) : "/", F.menu_items, n, on_menu, NULL);
}

/* ---- drawing (a ListSection with the breadcrumb, then the rows; no footer) ---- */
#define BAR_X (k->w - 3) /* the scrollbar's x (logical): the 3 px bar at the right edge, at any UI scale */

static kui_canvas_t S_KC NEMA_EXT_BSS;

static void draw_row(kui_canvas_t *k, int y, int w, int row, bool focus) {
    kui_list_item_t it = {0};
    const files_ent_t *e = row_ent(row);
    it.selected = focus;
    if (!e) {
        it.label = "..";
        it.icon = KUI_ICON_FOLDER;
        kui_list_item(k, 0, y, w, &it);
        return;
    }
    int kind = files_kind(e);
    char size[24] = "";
    it.label = e->name;
    it.icon = (kind == FILES_K_DIR || kind == FILES_K_ROOT) ? KUI_ICON_FOLDER : KUI_ICON_FILE;
    it.chevron = e->type != NEMA_T_FILE;
    if (e->type == NEMA_T_FILE) files_size_str(size, sizeof(size), e->size);
    if (e->type == FILES_T_ROOT) {
        char fs[16];
        files_size_str(fs, sizeof(fs), e->size);
        snprintf(size, sizeof(size), "%s free", fs);
    }
    it.value = size[0] ? size : NULL;
    kui_list_item(k, 0, y, w, &it);
}

static void draw_note(kui_canvas_t *k, int y, int w, const char *text) {
    kui_list_item_t it = {0};
    it.label = text;
    it.icon = -1;
    kui_list_item(k, 0, y, w, &it);
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&S_KC, c->w, c->h, kui_scale_q());
    kui_canvas_t *k = &S_KC;
    int ay = kui_lg(k, a.y), ah = kui_lg(k, a.y + a.h) - ay;
    kui_clip(k, 0, ay, k->w, ah);
    char head[NEMA_PATH_MAX];
    files_crumb(head, sizeof(head), F.cwd[0] ? F.cwd : "/");
    int y = ay + kui_section(k, 0, ay, &FONT_BOLD8, head);
    int list_h = ay + ah - y;
    int vis = list_h / KUI_LIST_PITCH;
    if (vis < 1) vis = 1;
    F.vis = vis;
    int total = rows_total();
    clamp_sel();
    int extra = (F.L.loading ? 1 : 0) + (F.L.truncated ? 1 : 0);
    int shown_rows = total + extra;
    bool bar = shown_rows > vis;
    int rw = k->w - (bar ? 3 : 0);
    for (int i = F.top; i < shown_rows; i++) {
        int ry = y + (i - F.top) * KUI_LIST_PITCH;
        if (ry >= ay + ah) break;
        if (i < total) draw_row(k, ry, rw, i, i == F.sel);
        else if (F.L.loading && i == total) draw_note(k, ry, rw, "Loading...");
        else draw_note(k, ry, rw, "More than 512 entries: the first 512 are shown");
    }
    if (total == 0 && !F.L.loading) draw_note(k, y, rw, "(empty)");
    if (bar) kui_scrollbar(k, BAR_X, y, list_h, F.top * KUI_LIST_PITCH, list_h, shown_rows * KUI_LIST_ROW_H + (shown_rows - 1) * 2);
    kui_present(c, k, a);
}

/* ---- the form (pal/menu.h): the listing as rows, for a session that draws forms in its own look. Two pages take turns, so a new
 * listing is a new page to the session even when it has as many rows as the last one. The selection is F.sel, copied in. ---- */
#define FORM_ROWS (FILES_MAX + 3)
static pal_menu_row_t s_rows[FORM_ROWS] NEMA_EXT_BSS;
static pal_menu_page_t s_page[2] NEMA_EXT_BSS;
static char s_title[2][NEMA_PATH_MAX] NEMA_EXT_BSS;
static int s_pi, s_built_n = -1;
static bool s_built_loading;
static pal_menu_t s_form NEMA_EXT_BSS;

static void form_value(void *ctx, char *out, size_t cap) {
    const files_ent_t *e = row_ent((int)(intptr_t)ctx);
    out[0] = 0;
    if (!e) return;
    if (e->type == NEMA_T_FILE) files_size_str(out, cap, e->size);
    else if (e->type == FILES_T_ROOT) {
        char fs[16];
        files_size_str(fs, sizeof(fs), e->size);
        snprintf(out, cap, "%s free", fs);
    }
}
static void form_run(void *ctx) {
    F.sel = (int)(intptr_t)ctx;
    if (!F.L.loading) activate();
}
static pal_menu_page_t *form_open(void *ctx) {
    form_run(ctx);
    return NULL; /* Files goes into the folder itself */
}
static void form_note(void *ctx, char *out, size_t cap) { snprintf(out, cap, "%s", (const char *)ctx); }

static void form_build(void) {
    s_pi ^= 1;
    pal_menu_page_t *p = &s_page[s_pi];
    files_crumb(s_title[s_pi], sizeof(s_title[s_pi]), F.cwd[0] ? F.cwd : "/");
    int n = 0, total = rows_total();
    for (int i = 0; i < total && n < FORM_ROWS - 2; i++) {
        const files_ent_t *e = row_ent(i);
        int kind = e ? files_kind(e) : FILES_K_DIR;
        bool dir = !e || kind == FILES_K_DIR || kind == FILES_K_ROOT;
        s_rows[n++] = (pal_menu_row_t){.kind = dir ? PAL_MENU_SUBMENU : PAL_MENU_ACTION, .label = e ? e->name : "..",
                                       .icon = dir ? "files" : kind == FILES_K_PACKAGE ? "package" : "text", .ctx = (void *)(intptr_t)i,
                                       .text = form_value, .run = form_run, .open = form_open, .show_value = true};
    }
    if (F.L.loading) s_rows[n++] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Loading...", .inert = true};
    else if (F.L.truncated)
        s_rows[n++] = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "More than 512 entries", .inert = true, .text = form_note,
                                       .ctx = (void *)"the first 512 are shown"};
    *p = (pal_menu_page_t){.title = s_title[s_pi], .rows = s_rows, .n = (size_t)n, .empty = "(empty)", .sel = F.sel};
    s_built_n = total, s_built_loading = F.L.loading;
    pal_menu_reset(&s_form, p);
    pal_menu_bind("files", &s_form);
}

/* Follows the listing: a new one (or more rows while loading) builds the form again; otherwise the selection is copied in. */
static void form_sync(void) {
    if (rows_total() != s_built_n || F.L.loading != s_built_loading) form_build();
    else s_page[s_pi].sel = F.sel;
}

/* ---- input, time ---- */
static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    int n = rows_total();
    switch (k->sym) {
    case PAL_KEY_UP: if (F.sel > 0) F.sel--; break;
    case PAL_KEY_DOWN: if (F.sel + 1 < n) F.sel++; break;
    case PAL_KEY_PGUP: F.sel -= F.vis > 1 ? F.vis - 1 : 1; if (F.sel < 0) F.sel = 0; break;
    case PAL_KEY_PGDN: F.sel += F.vis > 1 ? F.vis - 1 : 1; if (F.sel >= n) F.sel = n - 1; break;
    case PAL_KEY_HOME: F.sel = 0; break;
    case PAL_KEY_END: F.sel = n - 1; break;
    case PAL_KEY_ENTER:
        if (F.L.loading) return true;
        activate();
        break;
    case PAL_KEY_ESC:
        if (!has_up()) return false; /* roots: Esc goes home */
        go_up();
        break;
    case PAL_KEY_TAB:
        if (F.L.loading) return true;
        open_menu();
        return true;
    default: return false;
    }
    clamp_sel();
    form_sync();
    pal_desktop_invalidate();
    return true;
}

static void tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    if (F.L.loading) pal_desktop_invalidate();
}

static void pump(pal_view_t *v) {
    (void)v;
    if (F.want) {
        F.want = false;
        go(F.start);
    }
    if (F.L.loading) {
        int k = files_list_step(&F.L, LIST_PER_TICK);
        if (!F.L.loading) {
            on_loaded();
            pal_desktop_invalidate();
        } else if (k) {
            pal_desktop_invalidate();
        }
    }
    if (F.cwd[0]) form_sync();
    job_pump();
    if (F.defer && --F.defer == 0 && F.defer_act == A_INSTALL) {
        F.defer_act = A_NONE;
        run_install();
    }
}

static void enter(pal_view_t *v) {
    (void)v;
    if (!F.cwd[0]) go(F.start[0] ? F.start : "/home");
}

void pal_files_open(const char *path) {
    snprintf(F.start, sizeof(F.start), "%s", path && path[0] ? path : "/home");
    F.want = true;
    pal_desktop_open("files");
}

pal_view_t pal_files_view = {.id = "files", .title = "Files", .enter = enter, .draw = draw, .key = key, .pump = pump, .tick = tick, .tick_ms = 100};
