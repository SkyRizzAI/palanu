/* Athena's sheets: Keyboard (Fn twice), Keyboard shortcuts, Recent apps (cycle tap), and Options (the left softkey of a theme with
 * softkeys), a popup above the softkey bar of what can be done on the screen in front. */
#include "athena_int.h"

/* ---- sheets: Keyboard (Fn twice), Shortcuts, Recent apps (cycle tap) ---- */

static void style_row(int i) {
    bool on = !A.row_info && i == A.row_sel;
    ath_focus_bg(A.row[i], on, C_ROW);
    lv_obj_t *l = lv_obj_get_child(A.row[i], 0);
    lv_obj_set_style_text_color(l, on ? C_WHITE : C_INK, 0);
}

void ath_sheet_close(void) {
    if (A.kind == SHEET_NONE) return;
    A.kind = SHEET_NONE;
    lv_obj_add_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    A.force = true;
}

void ath_sheet_geometry(void) {
    lv_obj_set_pos(A.sheet, 8, BAR_H + 4);
    lv_obj_set_size(A.sheet, A.w - 16, A.h - BAR_H - 8 - ath_bottom());
    lv_obj_set_style_border_width(A.sheet, 0, 0);
}

static void sheet_open(sheet_t kind, const char *title, const char *const *rows, int n, bool info) {
    lv_obj_clean(A.sheet);
    ath_sheet_geometry();
    A.kind = kind;
    A.row_info = info;
    A.nrows = n > MAX_ROWS ? MAX_ROWS : n;
    A.row_sel = 0;
    A.sheet_title = ath_text(A.sheet, &lv_font_montserrat_20, C_INK);
    lv_label_set_text(A.sheet_title, title);
    lv_obj_set_pos(A.sheet_title, 14, 10);
    int rh = info ? 20 : 26, y = 40;
    for (int i = 0; i < A.nrows; i++) {
        lv_obj_t *r = lv_obj_create(A.sheet);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, A.w - 16 - 20, rh);
        lv_obj_set_pos(r, 10, y + i * (rh + 4));
        lv_obj_set_style_radius(r, ath_T->r_row, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_t *l = ath_text(r, info ? &lv_font_montserrat_12 : &lv_font_montserrat_16, C_INK);
        lv_label_set_text(l, rows[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        A.row[i] = r;
        style_row(i);
    }
    A.sheet_hint = ath_text(A.sheet, &lv_font_montserrat_12, C_GREY);
    lv_label_set_text(A.sheet_hint, ath_T->softkeys ? "" : info ? "Esc back" : "arrows choose  |  Enter open  |  Esc close");
    lv_obj_align(A.sheet_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_remove_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    A.force = true;
}

/* ---- Options (the left softkey, for a theme with softkeys): a popup above the softkey bar of what can be done here ---- */

typedef enum { OPT_MENU, OPT_OPEN, OPT_RECENT, OPT_KEYBOARD, OPT_SETTINGS, OPT_LOCK, OPT_SELECT, OPT_BACK, OPT_HOME, OPT_APPMENU } opt_t;

static const char *const OPT_LABEL[] = {"Menu", "Open", "Recent apps", "Keyboard", "Settings", "Lock", "Select", "Back", "Go to idle",
                                        "App menu"};

void ath_open_options(void) {
    uint8_t acts[MAX_ROWS];
    int n = 0;
    if (ath_page_menu() || ath_doc()) { /* a form or a document: its row, the app's own menu (its Tab), back, and the session's */
        acts[n++] = OPT_SELECT, acts[n++] = OPT_APPMENU, acts[n++] = OPT_BACK, acts[n++] = OPT_KEYBOARD, acts[n++] = OPT_HOME;
    } else if (A.menu_open) {
        if (A.n > 0) acts[n++] = OPT_OPEN;
        acts[n++] = OPT_RECENT, acts[n++] = OPT_KEYBOARD, acts[n++] = OPT_SETTINGS, acts[n++] = OPT_LOCK;
    } else {
        acts[n++] = OPT_MENU, acts[n++] = OPT_RECENT, acts[n++] = OPT_KEYBOARD, acts[n++] = OPT_SETTINGS, acts[n++] = OPT_LOCK;
    }
    lv_obj_clean(A.sheet);
    A.kind = SHEET_OPTIONS;
    A.row_info = false;
    A.nrows = n;
    A.row_sel = 0;
    const int RH = 22, W = A.w * 3 / 5, H = n * RH + 6;
    lv_obj_set_size(A.sheet, W, H);
    if (ath_T->popup_right) lv_obj_set_pos(A.sheet, A.w - W - 4, A.h - H - 6); /* at the right edge, near the trackpad */
    else lv_obj_set_pos(A.sheet, 2, A.h - ath_bottom() - H - 2);
    lv_obj_set_style_border_width(A.sheet, 1, 0);
    lv_obj_set_style_border_color(A.sheet, lv_color_hex(ath_T->pane_bot), 0);
    for (int i = 0; i < n; i++) {
        A.row_act[i] = acts[i];
        lv_obj_t *r = lv_obj_create(A.sheet);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, W - 6, RH);
        lv_obj_set_pos(r, 3, 3 + i * RH);
        lv_obj_set_style_radius(r, ath_T->r_row, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_t *l = ath_text(r, &lv_font_montserrat_16, C_INK);
        lv_label_set_text(l, OPT_LABEL[acts[i]]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        A.row[i] = r;
        style_row(i);
    }
    lv_obj_remove_flag(A.sheet, LV_OBJ_FLAG_HIDDEN);
    A.force = true;
}


static void run_option(opt_t o) {
    switch (o) {
    case OPT_MENU: ath_menu_show(true); break;
    case OPT_OPEN:
        if (A.n > 0) pal_launcher_run(&A.app[A.sel]);
        break;
    case OPT_RECENT: ath_open_recents_sheet(); break;
    case OPT_KEYBOARD: pal_vk_open(); break;
    case OPT_SETTINGS: pal_desktop_open("settings"); break;
    case OPT_LOCK: pal_desktop_lock(); break;
    case OPT_SELECT: (void)ath_send_key(PAL_KEY_ENTER); break; /* as Enter on the screen in front */
    case OPT_APPMENU: (void)ath_send_key(PAL_KEY_TAB); break;  /* the app's own menu key */
    case OPT_BACK:
        if (!ath_send_key(PAL_KEY_ESC)) { /* not used: an overlay closes, a screen goes home */
            pal_view_t *v = ath_app_view();
            if (v && v == pal_desktop_overlay_top()) pal_desktop_overlay_pop();
            else pal_desktop_home();
        }
        break;
    case OPT_HOME: pal_desktop_home(); break;
    }
}

void ath_open_keyboard_sheet(void) {
    static const char *const ROWS[] = {LV_SYMBOL_KEYBOARD "   On-screen keyboard", LV_SYMBOL_LIST "   Keyboard shortcuts",
                                       LV_SYMBOL_SETTINGS "   Settings", LV_SYMBOL_CLOSE "   Close"};
    sheet_open(SHEET_KEYBOARD, "Keyboard", ROWS, 4, false);
}

void ath_open_shortcuts_sheet(void) {
    static const char *const ROWS[] = {
        "Fn + W A S D        move (arrows)",  "Enter / Esc        open / back",       "Fn twice        Keyboard (this menu)",
        "Cycle tap        Recent apps",       "Cycle hold        Home",              "Ctrl + C        stop (in the Terminal)",
    };
    sheet_open(SHEET_SHORTCUTS, "Keyboard shortcuts", ROWS, 6, true);
}

void ath_open_recents_sheet(void) {
    static char labels[MAX_ROWS][40] NEMA_EXT_BSS;
    const char *rows[MAX_ROWS];
    int n = 0;
    for (int i = 0; i < desk_mru_count() && n < MAX_ROWS - 1; i++) {
        pal_view_t *v = desk_mru_at(i);
        if (!v || (v->flags & PAL_VIEW_HIDDEN) || !strcmp(v->id, "athena")) continue;
        nema_strlcpy(A.row_id[n], v->id, sizeof(A.row_id[n]));
        snprintf(labels[n], sizeof(labels[n]), "%s", v->title ? v->title : v->id);
        rows[n] = labels[n];
        n++;
    }
    nema_strlcpy(A.row_id[n], "athena", sizeof(A.row_id[n]));
    snprintf(labels[n], sizeof(labels[n]), "%s   Home", LV_SYMBOL_HOME);
    rows[n] = labels[n];
    n++;
    sheet_open(SHEET_RECENTS, "Recent apps", rows, n, false);
}

void ath_sheet_choose(void) {
    int i = A.row_sel;
    if (A.kind == SHEET_KEYBOARD) {
        ath_sheet_close();
        if (i == 0) pal_vk_open();
        else if (i == 1) ath_open_shortcuts_sheet();
        else if (i == 2) pal_desktop_open("settings");
    } else if (A.kind == SHEET_RECENTS) {
        ath_sheet_close();
        pal_desktop_open(A.row_id[i]);
    } else if (A.kind == SHEET_OPTIONS) {
        ath_sheet_close();
        run_option((opt_t)A.row_act[i]);
    }
}

void ath_sheet_move(int d) {
    if (A.row_info || A.nrows == 0) return;
    int old = A.row_sel;
    A.row_sel = (A.row_sel + d + A.nrows) % A.nrows;
    style_row(old);
    style_row(A.row_sel);
}


