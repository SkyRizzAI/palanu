/* The screens of the reference shots that PalanuOS has no action for, drawn with
 * kui: uishow screen NAME opens one of them. The dialogs use the reference's exact strings (a '\n' in them is a blank advance), so
 * they are not the PalanuOS dialog calls, which wrap their text. The permission and sign-consent screens are the reference
 * ComponentScreens: the permission one keeps the status bar, the sign-consent one is a modal over the whole screen. Dev aid
 * only; nothing in the product opens these. */
#include <string.h>

#include "nema/types.h"
#include "pal/desktop.h"
#include "pal/dialog.h"
#include "pal/input.h"
#include "pal/kui.h"
#include "pal/overlay.h"
#include "pal/toast.h"
#include "pal/ui.h"
#include "pal/theme.h"

#define SCR_PAD 8 /* The reference space.md */
#define SCR_GAP 4 /* The reference space.sm */

typedef struct {
    const char *title, *body, *cancel, *action;
} dlg_text_t;

enum page { PAGE_NONE, PAGE_BLANK, PAGE_POPUP, PAGE_PERMISSION, PAGE_SIGN };

static kui_canvas_t S NEMA_EXT_BSS;
static const dlg_text_t *cur_dlg;
static enum page cur_page;
static int perm_sel; /* 0 Deny, 1 Allow */

/* The reference Text centred in a column of w pixels at x0 (the column's padding is already in x0). */
static void ctext(const kui_font_t *f, int x0, int w, int y, const char *s) {
    int off = (w - kui_text_w(f, s)) / 2;
    kui_text(&S, f, x0 + (off > 0 ? off : 0), y, s, true);
}

static void dlg_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    if (!cur_dlg) return;
    const char *labels[2] = {cur_dlg->cancel, cur_dlg->action};
    kui_dialog(c, a, cur_dlg->title, cur_dlg->body, labels, 2, 0, false);
}

static void page_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&S, c->w, c->h, kui_scale_q());
    const int ay = kui_lg(&S, a.y); /* the layer's top, logical: the status bar's bottom, or 0 */
    if (cur_page == PAGE_POPUP) {
        /* The reference Popup, in a column the width of the screen: its padding, then the text centred */
        kui_popup(&S, ay + SCR_PAD, "Connected to MyHomeWiFi");
    } else if (cur_page == PAGE_SIGN) {
        /* The reference SignConsentScreen: the list fills the modal box, which is the whole screen here; its outline is drawn */
        kui_round(&S, 0, 0, S.w, S.h, 1, true);
        int y = kui_section(&S, 2, 2, &FONT_BOLD8, "Sign request");
        kui_list_item_t it = {0};
        it.icon = -1;
        it.label = "From";
        it.value = "https://app.example";
        kui_list_item(&S, 2, 2 + y + 2, S.w - 4, &it);
        it.label = "Key";
        it.value = "Software key";
        kui_list_item(&S, 2, 2 + y + 2 + 17, S.w - 4, &it);
        int cy = 2 + y + 2 + 17 + 17;
        cy += kui_section(&S, 2, cy, &FONT_BOLD8, "Confirm") + 2;
        it.value = NULL;
        it.label = "Reject";
        it.selected = true;
        kui_list_item(&S, 2, cy, S.w - 4, &it);
        it.selected = false;
        it.label = "Approve";
        kui_list_item(&S, 2, cy + 17, S.w - 4, &it);
    }
    if (cur_page != PAGE_NONE) {
        /* a page is the whole content area: paper, the canvas starts that way */
        kui_present(c, &S, a);
    }
}

/* The reference PermissionScreen is a modal of 210x100 (clamped to the 160x100 canvas), centred, over the screen below: the backdrop
 * is dithered from the status bar's bottom, the box holds a centred column (padding 8, gap 4) and the buttons (Deny focused). */
static void perm_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    kui_canvas_init(&S, c->w, c->h, kui_scale_q());
    kui_backdrop(c, a, kui_lg(&S, a.y), S.sq);
    const int mw = S.w < KUI_DIALOG_W ? S.w : KUI_DIALOG_W;
    const int mh = S.h < 100 ? S.h : 100;
    const int mx = (S.w - mw) / 2, my = (S.h - mh) / 2;
    kui_round(&S, mx, my, mw, mh, 1, true);
    const int x0 = mx + SCR_PAD, w = mw - 2 * SCR_PAD;
    int y = my + SCR_PAD;
    ctext(&FONT_REG8, x0, w, y, "Permission Request");
    y += FONT_REG8.charH + SCR_GAP;
    ctext(&FONT_BOLD10, x0, w, y, "\"terminal\"");
    y += FONT_BOLD10.charH + SCR_GAP;
    ctext(&FONT_REG8, x0, w, y, "wants to access");
    y += FONT_REG8.charH + SCR_GAP;
    ctext(&FONT_REG8, x0, w, y, "\"net.wifi.managed\"");
    y += FONT_REG8.charH + SCR_GAP;
    const char *labels[2] = {"Deny", "Allow"};
    kui_buttons(&S, x0, y, w, labels, 2, perm_sel);
    kui_present(c, &S, (pal_rect_t){kui_ph(S.sq, mx), kui_ph(S.sq, my), kui_ph(S.sq, mx + mw) - kui_ph(S.sq, mx), kui_ph(S.sq, my + mh) - kui_ph(S.sq, my)});
}

/* A fixture is a drawing, not a decision, but it is a modal like the real ones: it takes every key and leaves on Enter or Esc
 * (the permission one also moves between Deny and Allow), so a fixture left on the screen never traps the keyboard. */
static bool fixture_key(pal_view_t *v, const pal_key_t *k) {
    switch (k->sym) {
    case PAL_KEY_LEFT:
    case PAL_KEY_UP:
        if (v->draw == perm_draw && perm_sel != 0) perm_sel = 0, pal_desktop_invalidate_all();
        break;
    case PAL_KEY_RIGHT:
    case PAL_KEY_DOWN:
        if (v->draw == perm_draw && perm_sel != 1) perm_sel = 1, pal_desktop_invalidate_all();
        break;
    case PAL_KEY_TAB:
        if (v->draw == perm_draw) perm_sel ^= 1, pal_desktop_invalidate_all();
        break;
    case PAL_KEY_ENTER:
    case PAL_KEY_ESC: pal_desktop_overlay_remove(v); break;
    default: break;
    }
    return true;
}

static pal_view_t PERM_VIEW = {.id = "ref-permission", .title = "Permission", .draw = perm_draw, .key = fixture_key};
static pal_view_t DLG_VIEW = {.id = "ref-dialog", .title = "Dialog", .draw = dlg_draw, .key = fixture_key};
static pal_view_t PAGE_VIEW = {.id = "ref-page", .title = "Page", .draw = page_draw, .key = fixture_key};

static const dlg_text_t DLGS[] = {
    {"Eject SD Card", "Safely eject the\nSD card?", "Cancel", "Eject"},               /* eject-sd */
    {"Clear logs?", "Remove all log\nentries?", "Cancel", "Clear"},                  /* logs-clear */
    {"Stop server?", "Stop the display server.", "Cancel", "Stop"},          /* stop-server */
    {"Reboot Device?", "Reboot to USB\nbootloader mode.", "Cancel", "Reboot"},       /* reboot-bootloader */
};

static bool open_dialog(const dlg_text_t *d) {
    cur_dlg = d;
    cur_page = PAGE_NONE;
    return pal_desktop_overlay_push(&DLG_VIEW) == NEMA_OK;
}

static bool open_page(enum page p) {
    cur_page = p;
    cur_dlg = NULL;
    perm_sel = 0;
    if (p == PAGE_PERMISSION) return pal_desktop_overlay_push(&PERM_VIEW) == NEMA_OK; /* a modal over the screen below */
    /* the blank page, the popup and the permission screen keep the status bar; the sign-consent screen is full screen */
    PAGE_VIEW.flags = PAL_VIEW_OPAQUE | (p == PAGE_SIGN ? PAL_VIEW_NOBAR : 0);
    return pal_desktop_overlay_push(&PAGE_VIEW) == NEMA_OK;
}

bool pal_ui_ref_fixture(const char *name) {
    if (!name) return false;
    if (!strcmp(name, "eject-sd")) return open_dialog(&DLGS[0]);
    if (!strcmp(name, "logs-clear")) return open_dialog(&DLGS[1]);
    if (!strcmp(name, "stop-server")) return open_dialog(&DLGS[2]);
    if (!strcmp(name, "reboot-bootloader")) return open_dialog(&DLGS[3]);
    if (!strcmp(name, "files-delete")) {
        static const char *const items[2] = {"Delete", "Cancel"};
        return pal_picker_open("Delete \"readme.txt\"?", items, 2, NULL, NULL) == NEMA_OK;
    }
    if (!strcmp(name, "toast")) {
        bool ok = open_page(PAGE_BLANK);
        return ok && psys_toast("Settings saved", 10000) == NEMA_OK;
    }
    if (!strcmp(name, "popup")) return open_page(PAGE_POPUP);
    if (!strcmp(name, "permission")) return open_page(PAGE_PERMISSION);
    if (!strcmp(name, "sign-consent")) return open_page(PAGE_SIGN);
    return false;
}
