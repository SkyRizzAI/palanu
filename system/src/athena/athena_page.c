/* Athena: a bound menu (pal_menu_bind) drawn as a grouped list in full colour. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "athena_int.h"

/* ---- toolkit pages: a bound menu (Settings) drawn in Athena's look ---- */

pal_menu_t *ath_page_menu(void) {
    if (pal_desktop_overlay_count() || pal_desktop_locked()) return NULL;
    return pal_menu_bound(pal_desktop_front_id());
}

static void page_value(int i) {
    const pal_menu_row_t *r = &A.ppage->rows[A.pidx[i]];
    if (!A.pval[i]) return;
    char v[VALUE_MAX];
    if (r->kind == PAL_MENU_TOGGLE) { /* a switch: green when on, the knob moves */
        bool on = r->get && r->get(r->ctx);
        snprintf(v, sizeof(v), "%d", on);
        if (!strcmp(v, A.ptext[i])) return;
        nema_strlcpy(A.ptext[i], v, sizeof(A.ptext[i]));
        lv_obj_set_style_bg_color(A.pval[i], on ? C_GREEN : C_TRACK, 0);
        lv_obj_t *knob = lv_obj_get_child(A.pval[i], 0);
        lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
        return;
    }
    pal_menu_value_text(r, v, sizeof(v));
    if (r->kind == PAL_MENU_SLIDER && A.pbar[i] && r->get) { /* a slider or gauge: a bar next to its number */
        int span = r->max > r->min ? r->max - r->min : 1, x = r->get(r->ctx) - r->min;
        int pct = x < 0 ? 0 : x > span ? 100 : x * 100 / span;
        lv_obj_set_width(lv_obj_get_child(A.pbar[i], 0), (int32_t)(64 * pct / 100 < 2 ? 2 : 64 * pct / 100));
    }
    if (r->kind == PAL_MENU_SUBMENU || (r->kind == PAL_MENU_CHOICE)) {
        size_t n = strlen(v);
        snprintf(v + n, sizeof(v) - n, "%s%s", n ? "  " : "", LV_SYMBOL_RIGHT);
    }
    if (!strcmp(v, A.ptext[i])) return;
    nema_strlcpy(A.ptext[i], v, sizeof(A.ptext[i]));
    lv_label_set_text(A.pval[i], v);
}

/* A row's icon (the classic icon names the menus carry) as a glyph and a colour: in a rounded square (Cupertino), or the glyph alone in
 * the accent (Material). */
typedef struct {
    const char *name, *glyph;
    uint32_t color;
} row_icon_t;

static const row_icon_t ROW_ICONS[] = {
    {"brightness", LV_SYMBOL_IMAGE, 0x0A84FF},    {"moon", LV_SYMBOL_EYE_OPEN, 0x5E5CE6},   {"terminal", LV_SYMBOL_KEYBOARD, 0x8E8E93},
    {"wifi", LV_SYMBOL_WIFI, 0x0A84FF},           {"bluetooth", LV_SYMBOL_BLUETOOTH, 0x0A84FF}, {"link", LV_SYMBOL_LOOP, 0x34C759},
    {"default", LV_SYMBOL_SETTINGS, 0x8E8E93},    {"notifications", LV_SYMBOL_BELL, 0xFF3B30}, {"files", LV_SYMBOL_DIRECTORY, 0x0A84FF},
    {"store", LV_SYMBOL_DOWNLOAD, 0x0A84FF},      {"lock", LV_SYMBOL_EYE_CLOSE, 0xFF3B30},  {"clock", LV_SYMBOL_REFRESH, 0x8E8E93},
    {"power", LV_SYMBOL_POWER, 0x34C759},         {"monitor", LV_SYMBOL_EDIT, 0x5856D6},    {"info", LV_SYMBOL_LIST, 0x8E8E93},
    {"about", LV_SYMBOL_LIST, 0x8E8E93},          {"settings", LV_SYMBOL_SETTINGS, 0x8E8E93}, {"battery", LV_SYMBOL_BATTERY_FULL, 0x34C759},
    {"package", LV_SYMBOL_FILE, 0xFF9500},        {"apps", LV_SYMBOL_HOME, 0x5E5CE6},       {"logs", LV_SYMBOL_FILE, 0xFF9500},
    {"services", LV_SYMBOL_SHUFFLE, 0x30B0C7},    {"text", LV_SYMBOL_EDIT, 0x8E8E93},
};

static const row_icon_t *row_icon(const char *name) {
    for (size_t i = 0; name && i < sizeof(ROW_ICONS) / sizeof(ROW_ICONS[0]); i++)
        if (!strcmp(ROW_ICONS[i].name, name)) return &ROW_ICONS[i];
    return NULL;
}

/* The selection: the accent fills the row, the text turns white (a keyboard needs a focus that shows at a glance). */
static void page_style_row(int i) {
    const pal_menu_row_t *r = &A.ppage->rows[A.pidx[i]];
    if (r->kind == PAL_MENU_SECTION) return;
    bool on = A.pidx[i] == A.psel;
    ath_focus_bg(A.prow[i], on, C_ROW);
    lv_color_t ink = r->disabled ? C_GREY : r->danger ? C_RED : C_INK; /* a row that cannot be used looks it, even a destructive one */
    lv_obj_set_style_text_color(A.plabel[i], on ? C_WHITE : ink, 0);
    if (A.pval[i] && r->kind != PAL_MENU_TOGGLE) lv_obj_set_style_text_color(A.pval[i], on ? C_WHITE : C_GREY, 0);
    if (A.picon[i]) lv_obj_set_style_text_color(A.picon[i], on ? C_WHITE : C_BLUE, 0);
}

#define ROW_H 30

/* The title of the page under this one, for the back button (Cupertino). */
static const char *parent_title(void) {
    int d = pal_menu_depth(A.pm);
    if (d < 2) return "";
    pal_menu_page_t *p = A.pm->stack[d - 2];
    if (p->title && p->title[0]) return p->title;
    pal_view_t *fv = pal_desktop_view(pal_desktop_front_id());
    return fv && fv->title ? fv->title : "Back";
}

/* The page row at the top of a page to read: the menu scrolls in logical rows of about 13 px (menu_draw.c). */
static int info_anchor(const pal_menu_page_t *p) { return p->spx / 13; }

static void page_build(void) {
    lv_obj_clean(A.page_list);
    A.pn = 0;
    A.psel = A.ppage->sel;
    A.prows = A.ppage->n;
    const char *title = A.ppage->title && A.ppage->title[0] ? A.ppage->title : NULL;
    if (!title) { /* a root page without a title: the app's name */
        pal_view_t *fv = pal_desktop_view(pal_desktop_front_id());
        title = fv && fv->title ? fv->title : "";
    }
    bool grouped = !ath_T->flat, sub = pal_menu_depth(A.pm) > 1;
    if (grouped) { /* a navigation bar: the title in the middle, the page below as a blue back button */
        lv_label_set_text(A.page_title, title);
        lv_obj_align(A.page_title, LV_ALIGN_TOP_MID, 0, 6);
        if (sub) {
            /* the back label stops short of the centred title: a long parent ends in dots */
            lv_point_t tsz;
            lv_text_get_size(&tsz, title, lv_obj_get_style_text_font(A.page_title, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int room = (A.w - tsz.x) / 2 - 8 - 6;
            lv_label_set_long_mode(A.page_back, LV_LABEL_LONG_DOT);
            lv_obj_set_size(A.page_back, room > 24 ? room : 24, 20);
            lv_label_set_text_fmt(A.page_back, LV_SYMBOL_LEFT " %s", parent_title());
        } else {
            lv_label_set_text(A.page_back, "");
        }
    } else if (ath_T->title_pane) { /* the title on the pane; Back is the right softkey's */
        lv_label_set_text(A.page_title, title);
        lv_obj_align(A.page_title, LV_ALIGN_LEFT_MID, 8, 0);
        lv_label_set_text(A.page_back, "");
    } else {
        lv_label_set_text_fmt(A.page_title, "%s%s", sub ? LV_SYMBOL_LEFT "  " : "", title);
        lv_obj_set_pos(A.page_title, 14, 4);
        lv_label_set_text(A.page_back, "");
    }
    /* Cupertino: rows in rounded cards, one per section, with hairlines between them; Material: rows edge to edge */
    int inset = grouped ? 12 : 0, w = A.w - 2 * inset, y = grouped ? 6 : 2;
    lv_obj_t *card = NULL, *prev = NULL;
    int card_y = 0, card_n = 0;
    /* a long page (a big folder) is drawn as a window of MAX_PAGE_ROWS rows around the selection, built again when it leaves it */
    /* a page with nothing to select (rows to read) puts its window around where the keys scrolled it instead */
    size_t start = 0;
    bool info = !pal_menu_has_selectable(A.ppage);
    if (A.ppage->n > MAX_PAGE_ROWS) {
        int st = info ? info_anchor(A.ppage) - 4 : A.psel - MAX_PAGE_ROWS / 2, last = (int)A.ppage->n - MAX_PAGE_ROWS;
        start = (size_t)(st < 0 ? 0 : st > last ? last : st);
    }
    A.pstart = (int)start;
    for (size_t k = start; k < A.ppage->n && A.pn < MAX_PAGE_ROWS; k++) {
        const pal_menu_row_t *r = &A.ppage->rows[k];
        if (r->kind == PAL_MENU_SECTION && r->group) { /* a new card without a title */
            if (card) y = card_y + card_n * ROW_H + 14;
            card = prev = NULL;
            card_n = 0;
            continue;
        }
        if (pal_menu_row_hidden(r)) continue;
        int i = A.pn++;
        A.pidx[i] = (int)k;
        A.pval[i] = NULL;
        A.pbar[i] = NULL;
        A.picon[i] = NULL;
        A.ptext[i][0] = 0;
        if (r->kind == PAL_MENU_SECTION) { /* a small grey header above its group; it closes the card before it */
            if (card) y = card_y + card_n * ROW_H + 10;
            card = prev = NULL;
            card_n = 0;
            lv_obj_t *hdr = lv_obj_create(A.page_list);
            lv_obj_remove_style_all(hdr);
            lv_obj_set_size(hdr, w, 18);
            lv_obj_set_pos(hdr, inset, y);
            A.prow[i] = hdr;
            lv_obj_t *l = ath_text(hdr, &lv_font_montserrat_12, C_GREY);
            char up[40];
            nema_strlcpy(up, r->label ? r->label : "", sizeof(up));
            for (char *c = up; grouped && *c; c++) *c = (char)toupper((unsigned char)*c);
            lv_label_set_text(l, up);
            lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, grouped ? 12 : 10, 0);
            A.plabel[i] = l;
            y += 20;
            continue;
        }
        lv_obj_t *parent = A.page_list;
        int ry = y;
        if (grouped) {
            if (!card) {
                card = lv_obj_create(A.page_list);
                lv_obj_remove_style_all(card);
                lv_obj_set_pos(card, inset, y);
                lv_obj_set_width(card, w);
                lv_obj_set_style_radius(card, ath_T->r_card, 0);
                lv_obj_set_style_bg_color(card, C_ROW, 0);
                lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
                lv_obj_set_style_clip_corner(card, true, 0);
                lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
                card_y = y, card_n = 0;
            }
            if (prev) { /* a hairline under the row before, from its text to the right edge */
                lv_obj_t *line = lv_obj_create(prev);
                lv_obj_remove_style_all(line);
                lv_obj_set_size(line, w - 44, 1);
                lv_obj_align(line, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
                lv_obj_set_style_bg_color(line, lv_color_hex(0xC6C6C8), 0);
                lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            }
            parent = card;
            ry = card_n * ROW_H;
            card_n++;
            lv_obj_set_height(card, card_n * ROW_H);
        } else {
            y += ROW_H;
        }
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, ROW_H);
        lv_obj_set_pos(row, grouped ? 0 : inset, grouped ? ry : y - ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        A.prow[i] = row;
        prev = row;
        int text_x = 12;
        const row_icon_t *ic = row_icon(r->icon);
        if (ic) {
            if (grouped) { /* a white glyph on a small rounded colour square */
                lv_obj_t *sq = lv_obj_create(row);
                lv_obj_remove_style_all(sq);
                lv_obj_set_size(sq, 20, 20);
                lv_obj_align(sq, LV_ALIGN_LEFT_MID, 10, 0);
                lv_obj_set_style_radius(sq, 4, 0);
                lv_obj_set_style_bg_color(sq, lv_color_hex(ic->color), 0);
                lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
                lv_obj_t *g = ath_text(sq, &lv_font_montserrat_12, C_WHITE);
                lv_label_set_text(g, ic->glyph);
                lv_obj_center(g);
            } else { /* the glyph in the accent, no square (white on the selected row) */
                lv_obj_t *g = ath_text(row, &lv_font_montserrat_16, C_BLUE);
                A.picon[i] = g;
                lv_label_set_text(g, ic->glyph);
                lv_obj_align(g, LV_ALIGN_LEFT_MID, 14, 0);
            }
            text_x = grouped ? 40 : 44;
        }
        lv_obj_t *l = ath_text(row, &lv_font_montserrat_16, C_INK);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_size(l, w / 2 + 30 - text_x + 12, 19); /* one line: a long name ends in dots */
        lv_label_set_text(l, r->label ? r->label : "");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, text_x, 0);
        A.plabel[i] = l;
        if (r->kind == PAL_MENU_TOGGLE) {
            lv_obj_t *sw = lv_obj_create(row);
            lv_obj_remove_style_all(sw);
            lv_obj_set_size(sw, 36, 20);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -10, 0);
            lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_t *knob = lv_obj_create(sw);
            lv_obj_remove_style_all(knob);
            lv_obj_set_size(knob, 16, 16);
            lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(knob, C_WHITE, 0);
            lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
            A.pval[i] = sw;
        } else {
            if (r->kind == PAL_MENU_SLIDER) { /* a track with a fill, left of the number */
                lv_obj_t *track = lv_obj_create(row);
                lv_obj_remove_style_all(track);
                lv_obj_set_size(track, 64, 6);
                lv_obj_align(track, LV_ALIGN_RIGHT_MID, -44, 0);
                lv_obj_set_style_radius(track, 3, 0);
                lv_obj_set_style_bg_color(track, C_TRACK, 0);
                lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
                lv_obj_t *fill = lv_obj_create(track);
                lv_obj_remove_style_all(fill);
                lv_obj_set_size(fill, 2, 6);
                lv_obj_set_style_radius(fill, 3, 0);
                lv_obj_set_style_bg_color(fill, C_BLUE, 0);
                lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
                A.pbar[i] = track;
            }
            lv_obj_t *v = ath_text(row, &lv_font_montserrat_12, C_GREY);
            lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_size(v, w / 2 - 40, 15); /* one line: a long value ends in dots inside its row */
            lv_label_set_text(v, "");
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
            A.pval[i] = v;
        }
        page_value(i);
        page_style_row(i);
    }
    if (A.pn == 0) lv_label_set_text(A.page_empty, A.ppage->empty ? A.ppage->empty : "Nothing here");
    else lv_label_set_text(A.page_empty, "");
    for (int i = 0; i < A.pn; i++)
        if (A.pidx[i] == A.psel) lv_obj_scroll_to_view_recursive(A.prow[i], LV_ANIM_OFF);
    if (info && start > 0 && pal_menu_scrolled_to_end(A.pm)) { /* at the end: the last row shows, whatever the rows' heights */
        lv_obj_update_layout(A.page_list);
        lv_obj_scroll_by_bounded(A.page_list, 0, -lv_obj_get_scroll_bottom(A.page_list), LV_ANIM_OFF);
    } else if (info && start > 0) { /* the row the scroll had reached goes to the top */
        int anchor = info_anchor(A.ppage);
        for (int i = 0; i < A.pn; i++)
            if (A.pidx[i] >= anchor) {
                lv_obj_update_layout(A.page_list);
                lv_obj_scroll_to_y(A.page_list, lv_obj_get_y(A.prow[i]) - 4, LV_ANIM_OFF);
                break;
            }
    }
}

/* Follows the bound menu: a new page or row count rebuilds, a new selection restyles, values refresh after a key and twice a second. */
/* What the rows say: kind, label text, icon, whether they show, are disabled or destructive, and the title. A page that builds
 * its rows again (a Wi-Fi scan, the PIN page after a change) keeps its count and its address, so this is what tells it moved. */
static uint32_t rows_sig(const pal_menu_page_t *p) {
    uint32_t h = 2166136261u;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    for (const char *c = p->title; c && *c; c++) MIX((unsigned char)*c);
    for (size_t k = 0; k < p->n; k++) {
        const pal_menu_row_t *r = &p->rows[k];
        MIX(r->kind);
        MIX(pal_menu_row_hidden(r) | r->disabled << 1 | r->danger << 2 | r->inert << 3 | r->group << 4);
        MIX((uintptr_t)r->icon);
        for (const char *c = r->label; c && *c; c++) MIX((unsigned char)*c);
    }
#undef MIX
    return h;
}

void ath_page_sync(pal_menu_t *m, uint32_t now) {
    pal_menu_page_t *p = pal_menu_top(m);
    if (!p) return;
    uint32_t sig = rows_sig(p);
    if (m != A.pm || p != A.ppage || p->n != A.prows || sig != A.prsig) {
        A.pm = m;
        A.ppage = p;
        A.prsig = sig;
        page_build();
        A.pspx = p->spx;
        A.pvals_ms = now;
        return;
    }
    if (p->spx != A.pspx) { /* the keys scrolled the page without moving the selection (rows to read, or none to select) */
        int d = p->spx - A.pspx;
        A.pspx = p->spx;
        if (p->n > MAX_PAGE_ROWS && !pal_menu_has_selectable(p)) { /* a long page to read: near an edge of its window, move it */
            int anchor = info_anchor(p), last = (int)p->n - MAX_PAGE_ROWS;
            if ((anchor < A.pstart + 2 && A.pstart > 0) || (anchor + 12 > A.pstart + MAX_PAGE_ROWS && A.pstart < last)) {
                page_build();
                return;
            }
        }
        if (p->spx == 0) lv_obj_scroll_to_y(A.page_list, 0, LV_ANIM_OFF);
        else {
            int px = (d < 0 ? -d : d) * ROW_H / 13; /* the menu scrolls in its logical rows of about 13 px */
            if (px < ROW_H) px = ROW_H;
            if (d > 0 && pal_menu_scrolled_to_end(m)) px = lv_obj_get_scroll_bottom(A.page_list); /* the end: all of it */
            lv_obj_scroll_by_bounded(A.page_list, 0, d > 0 ? -px : px, LV_ANIM_OFF);
        }
    }
    if (p->sel != A.psel) {
        int old = A.psel;
        A.psel = p->sel;
        bool drawn = false;
        for (int i = 0; i < A.pn && !drawn; i++) drawn = A.pidx[i] == A.psel;
        if (!drawn) { /* outside the window of a long page */
            page_build();
            return;
        }
        for (int i = 0; i < A.pn; i++)
            if (A.pidx[i] == old || A.pidx[i] == A.psel) {
                page_style_row(i);
                if (A.pidx[i] == A.psel) lv_obj_scroll_to_view_recursive(A.prow[i], LV_ANIM_OFF);
            }
    }
    /* after a key (a toggle, a choice) at once; values that change on their own twice a second */
    if (m->changes != A.pchanges || now - A.pvals_ms >= 500u) {
        A.pchanges = m->changes;
        A.pvals_ms = now;
        for (int i = 0; i < A.pn; i++) page_value(i);
    }
}

void ath_build_page(void) {
    A.page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(A.page);
    lv_obj_set_pos(A.page, 0, BAR_H);
    lv_obj_set_size(A.page, A.w, A.h - BAR_H - ath_bottom());
    lv_obj_set_style_bg_color(A.page, C_SHEET, 0);
    lv_obj_set_style_bg_opa(A.page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(A.page, LV_OBJ_FLAG_SCROLLABLE);
    int list_y = 32;
    A.page_pane = NULL;
    if (ath_T->title_pane) { /* a band in the pane colours across the top, the title white on it */
        list_y = 26;
        A.page_pane = lv_obj_create(A.page);
        lv_obj_remove_style_all(A.page_pane);
        lv_obj_set_size(A.page_pane, A.w, 24);
        ath_pane_bg(A.page_pane);
        lv_obj_remove_flag(A.page_pane, LV_OBJ_FLAG_SCROLLABLE);
        A.page_title = ath_text(A.page_pane, &lv_font_montserrat_16, C_WHITE);
        lv_label_set_long_mode(A.page_title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(A.page_title, A.w - 16);
    } else {
        A.page_title = ath_text(A.page, ath_T->flat ? &lv_font_montserrat_20 : &lv_font_montserrat_16, C_INK);
    }
    lv_obj_set_pos(A.page_title, 14, 4);
    A.page_back = ath_text(A.page, &lv_font_montserrat_16, C_BLUE); /* Cupertino: the page below, as a back button */
    lv_obj_set_pos(A.page_back, 8, 6);
    lv_label_set_text(A.page_back, "");
    A.page_list = lv_obj_create(A.page);
    lv_obj_remove_style_all(A.page_list);
    lv_obj_set_pos(A.page_list, 0, list_y);
    lv_obj_set_size(A.page_list, A.w, A.h - BAR_H - ath_bottom() - list_y);
    lv_obj_add_flag(A.page_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(A.page_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(A.page_list, 10, 0); /* the last row keeps a gap above the screen edge when scrolled into view */
    A.page_empty = ath_text(A.page, &lv_font_montserrat_16, C_GREY);
    lv_label_set_text(A.page_empty, "");
    lv_obj_align(A.page_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(A.page, LV_OBJ_FLAG_HIDDEN);
}

