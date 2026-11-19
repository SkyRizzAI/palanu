/* The desktop (the reference DesktopScreen with LiveWallpaperDesktop). The animation wallpaper (ui.wallpaper = anim:NAME, fitted with
 * ui.wall_fit and ui.wall_anchor) fills the whole screen: the home is fullscreen, so the system status bar is not drawn here.
 * Instead, floating badges run along the top (Wi-Fi, BLE, Link, battery, unread count) as the pre-port home had them, and a big
 * clock and the date sit over the wallpaper. Its legends are the legend bar at the bottom (pal/legend.h): OK Apps (the
 * launcher) and Up Control.
 * The picture is drawn on a logical canvas and expanded to the panel by kui_present, as the reference does.
 *
 * Only the animation is drawn here: the reference has one desktop skin, so the other wallpapers of the old home (stars, city, waves)
 * are gone. The wallpaper is paper when ui.wallpaper is not the animation. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/anim.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/dpm.h"
#include "pal/icons.h"
#include "pal/kui.h"
#include "pal/link.h"
#include "pal/notify.h"
#include "pal/radio.h"
#include "pal/sysinfo.h"
#include "pal/time.h"
#include "pal/ui.h"
#include "pal/wifi.h"

#define HOME_TICK_MS 100
#define BADGE_Y 2     /* the badge row, logical */
#define BADGE_H 10    /* a badge: the 8 px icon and a pixel of air above and below */
#define BADGE_GAP 3   /* between badges; Wi-Fi, BLE and Link are icon-only so the row fits the 160 px logical width */
#define CLOCK_SCALE 2 /* the Bold 12 clock at twice its size */
#define CLOCK_Y 23    /* the old home's clock top, in logical pixels */

/* Legends for the legend bar: {action, label}. Two fit the 160 px bar in the 6x8 font with hhv2's caps (ENTER Apps,
 * Fn+W Control); Down for the notifications is left out for room. The home draws no hint line itself; the bar reads this table. */

static const pal_legend_item_t HOME_LEGENDS[] = {
    {PAL_ACT_OK, "Apps", NULL}, {PAL_ACT_UP, "Control", NULL}};

static struct {
    bool visible;
    pal_anim_t *anim;  /* the wallpaper's file or built-in animation, NULL when none */
    char anim_name[PAL_ANIM_NAME_MAX];
    pal_anim_player_t player;
    bool frozen;       /* the golden tool holds the playhead at freeze */
    int freeze;
    uint32_t now_ms;   /* the last tick's clock */
    int last_min;      /* the clock's minute when last painted */
    int chip_sig;      /* what the badges showed last */
    bool was_off;      /* the display was off at the last tick: the desktop repaints on wake */
    kui_canvas_t kc;   /* the logical canvas the desktop is drawn into */
    uint8_t mono[PAL_ANIM_MAX_W * PAL_ANIM_MAX_H / 8]; /* an indexed frame converted to mono bits */
} H NEMA_EXT_BSS; /* zero at boot (external RAM where the board has it); enter() sets last_min and chip_sig */

static uint32_t now_ms(void) { return (uint32_t)(pal_sys_uptime_us() / 1000u); }

/* The ui.wall_fit setting (0 center, 1 fit, 2 stretch, 3 crop) as the reference fit enum. */
static int kui_fit(void) {
    switch (pal_ui_wall_fit()) {
    case 0: return KUI_FIT_CENTER;
    case 2: return KUI_FIT_STRETCH;
    case 3: return KUI_FIT_CROP;
    default: return KUI_FIT_FIT;
    }
}

/* Opens the wallpaper's animation (a built-in one, else a file), or closes the one open. */
static void anim_sync(uint32_t t) {
    if (H.anim) pal_anim_close(H.anim);
    H.anim = NULL;
    H.anim_name[0] = 0;
    if (pal_ui_wallpaper() != PAL_UI_WALL_ANIM) return;
    const char *name = pal_ui_wall_anim();
    nema_strlcpy(H.anim_name, name, sizeof(H.anim_name));
    H.anim = pal_anim_open_builtin(name);
    if (!H.anim) H.anim = pal_anim_open_file(name);
    if (H.anim) pal_anim_play_start(&H.player, H.anim, t);
}

/* ---- the badges: one pill per status, an 8 px icon and its label; on = inverted (ink) ---- */

static int battery_level(int pct) {
    if (pct < 0) return 0;
    int v = (pct * 4 + 50) / 100;
    return v > 4 ? 4 : v;
}

static int unread(void) {
    static psys_notify_t buf[PSYS_NOTIFY_RING] NEMA_EXT_BSS;
    int n = psys_notify_list(buf, PSYS_NOTIFY_RING), k = 0;
    for (int i = 0; i < n; i++)
        if (!buf[i].read) k++;
    return k;
}

static int chip_signature(void) {
    int bars = pal_wifi_bars(), ble = pal_radio_state(PAL_RADIO_BLE) == PAL_RADIO_ON;
    int link = pal_link_connected(), pct = pal_battery(), un = unread();
    return ((bars * 2 + ble) * 2 + link) * 100000 + (pct + 1) * 64 + (un > 63 ? 63 : un);
}

/* One badge at x: a rounded pill of the 8 px icon, a gap, and the label in Reg 8. Returns its width. */
static int badge(kui_canvas_t *k, int x, const char *icon, const char *label, bool on) {
    const kui_font_t *f = kui_slot(KUI_F_REG8);
    const pal_icon_t *ic = pal_icon_find(icon, 8);
    int iw = ic ? ic->w : 0;
    int lw = label && *label ? kui_text_w(f, label) : 0;
    int pad = 2, gap = iw && lw ? 2 : 0;
    int w = pad + iw + gap + lw + pad;
    kui_fill_round(k, x, BADGE_Y, w, BADGE_H, 2, on);
    if (!on) kui_round(k, x, BADGE_Y, w, BADGE_H, 2, true);
    if (ic) kui_bitmap(k, x + pad, BADGE_Y + (BADGE_H - ic->h) / 2, ic->w, ic->h, ic->bits, !on);
    if (lw) kui_text(k, f, x + pad + iw + gap, BADGE_Y + (BADGE_H - f->charH) / 2, label, !on);
    return w;
}

/* Wi-Fi, BLE and Link (icons), battery (icon and percent) and the unread count, left to right; each is inverted while it
 * is active. */
static void badges(kui_canvas_t *k) {
    int bars = pal_wifi_bars(), pct = pal_battery(), un = unread();
    bool ble = pal_radio_state(PAL_RADIO_BLE) == PAL_RADIO_ON;
    char label[12], icon[12];
    if (bars < 0) bars = 0;
    if (bars > 4) bars = 4;
    snprintf(icon, sizeof(icon), "wifi%d", bars);
    int x = 2;
    x += badge(k, x, icon, NULL, bars > 0) + BADGE_GAP;
    x += badge(k, x, "bluetooth", NULL, ble) + BADGE_GAP;
    x += badge(k, x, "link", NULL, pal_link_connected()) + BADGE_GAP;
    if (pct >= 0) snprintf(label, sizeof(label), "%d%%", pct);
    else snprintf(label, sizeof(label), "--");
    snprintf(icon, sizeof(icon), "battery%d", battery_level(pct));
    x += badge(k, x, icon, label, false) + BADGE_GAP;
    snprintf(label, sizeof(label), "%d", un);
    badge(k, x, "bell", label, un > 0);
}

/* The big clock and the date, centred. Over the animation a paper plate keeps both readable. */
static void clock_block(kui_canvas_t *k, bool plate) {
    pal_tm_t tm;
    pal_time_local(&tm);
    char hm[8], date[32];
    pal_ui_fmt_hm(tm.hour, tm.minute, hm, sizeof(hm));
    pal_ui_fmt_date(&tm, date, sizeof(date));
    const kui_font_t *big = kui_slot(KUI_F_BOLD12), *df = kui_slot(KUI_F_REG8);
    /* the big clock at twice the font, unless the canvas is narrower than the design's (the largest UI scales) */
    const int cs = k->w >= 150 ? CLOCK_SCALE : 1;
    int w = kui_text_w_scaled(big, hm, cs), dw = kui_text_w(df, date);
    int bw = w > dw ? w : dw, date_y = CLOCK_Y + big->charH * cs + 4;
    int bx = (k->w - bw) / 2;
    if (plate) kui_fill_round(k, bx - 4, CLOCK_Y - 4, bw + 8, date_y + df->charH + 4 - (CLOCK_Y - 4), 2, false);
    kui_text_scaled(k, big, (k->w - w) / 2, CLOCK_Y, hm, cs, true);
    const char *ampm = pal_ui_ampm(tm.hour); /* a 12-hour clock: the suffix small, at the big clock's baseline */
    if (ampm[0]) kui_text(k, df, (k->w + w) / 2 + 2, CLOCK_Y + big->charH * cs - df->charH, ampm, true);
    kui_text(k, df, (k->w - dw) / 2, date_y, date, true);
}

/* One repaint of the whole screen from scratch: paper, the wallpaper frame, the badges and the clock. */
static void paint(pal_canvas_t *c, pal_rect_t a) {
    kui_canvas_init(&H.kc, c->w, c->h, kui_scale_q()); /* all paper */
    kui_canvas_t *k = &H.kc;
    int cy = kui_lg(k, a.y), ch = kui_lg(k, a.y + a.h) - cy; /* the content area in logical pixels (the reference contentY) */
    bool anim = H.anim != NULL;
    if (anim) {
        const pal_anim_info_t *inf = pal_anim_info(H.anim);
        const uint8_t *bits = pal_anim_mono_bits(H.anim, pal_anim_play_bitmap(&H.player), H.mono, sizeof(H.mono));
        if (bits) kui_wall_fit(k, bits, inf->width, inf->height, 0, cy, k->w, ch, kui_fit(), pal_ui_wall_anchor());
    }
    clock_block(k, anim);
    badges(k);
    kui_present(c, k, a);
}

/* ---- the view ---- */

static void enter(pal_view_t *v) {
    (void)v;
    H.visible = true;
    H.now_ms = now_ms();
    H.last_min = -1;
    H.chip_sig = -1;
    anim_sync(H.now_ms);
}

static void leave(pal_view_t *v) {
    (void)v;
    H.visible = false;
    if (H.anim) pal_anim_close(H.anim);
    H.anim = NULL;
}

/* An overlay (a dialog, a toast) covers the desktop: the wallpaper stops until it is exposed again. */
static void obscured(pal_view_t *v) {
    (void)v;
    H.visible = false;
}

/* The layer above closed: the desktop comes back as it did from the launcher. */
static void exposed(pal_view_t *v) {
    (void)v;
    H.visible = true;
    H.now_ms = now_ms();
    pal_desktop_invalidate_all();
}

static void draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    pal_tm_t tm;
    pal_time_local(&tm);
    H.last_min = tm.minute;
    H.chip_sig = chip_signature();
    if (H.frozen && H.anim) pal_anim_play_seek(&H.player, H.freeze);
    paint(c, a);
}


static void tick(pal_view_t *v, uint32_t t) {
    (void)v;
    if (!H.visible) return;
    H.now_ms = t;
    if (!pal_dpm_display_on()) { /* asleep: nothing is painted; the desktop repaints on wake */
        H.was_off = true;
        return;
    }
    pal_rect_t a = pal_desktop_area(&pal_home_view);
    pal_canvas_t *c = pal_display_canvas();
    if (H.was_off) {
        H.was_off = false;
        paint(c, a);
        return;
    }
    bool want_anim = pal_ui_wallpaper() == PAL_UI_WALL_ANIM;
    bool other = want_anim ? strcmp(pal_ui_wall_anim(), H.anim_name) != 0 : H.anim != NULL;
    if (other) {
        /* a new wallpaper from the settings: reopen it and repaint all of it */
        anim_sync(t);
        paint(c, a);
        return;
    }
    bool changed = false;
    if (H.anim) {
        if (H.frozen) { /* frozen: move only to the position the golden tool asked for */
            int before = pal_anim_play_bitmap(&H.player);
            pal_anim_play_seek(&H.player, H.freeze);
            changed = pal_anim_play_bitmap(&H.player) != before;
        } else {
            changed = pal_anim_play_tick(&H.player, t);
        }
    }
    pal_tm_t tm;
    pal_time_local(&tm);
    if (tm.minute != H.last_min) {
        H.last_min = tm.minute;
        changed = true;
    }
    int sig = chip_signature();
    if (sig != H.chip_sig) {
        H.chip_sig = sig;
        changed = true;
    }
    if (changed) paint(c, a);
}

static bool key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    switch (k->sym) {
    case PAL_KEY_ENTER: pal_desktop_open("launcher"); return true;
    case PAL_KEY_UP: pal_desktop_open("control"); return true;      /* the reference: Missions */
    case PAL_KEY_DOWN: pal_desktop_open("notifications"); return true;
    case PAL_KEY_ESC: return true;
    default: return false;
    }
}

/* Any session, any task: the lock is the running session's lock screen, home its home, any other id a view it has registered.
 * The calls below queue the change for the desktop task. */
bool pal_ui_request_view(const char *id) {
    if (!id) return false;
    if (!strcmp(id, "lock")) return pal_desktop_lock() == NEMA_OK;
    if (!strcmp(id, "home")) {
        if (!pal_desktop_home_id()[0]) return false;
        pal_desktop_home();
        return true;
    }
    if (!pal_desktop_view(id)) return false; /* not a screen of the running session */
    pal_desktop_open(id);
    return true;
}

void pal_ui_home_freeze(int frame) {
    H.frozen = frame >= 0;
    H.freeze = frame;
    pal_desktop_invalidate_all();
}

pal_view_t pal_home_view = {
    .id = "home", .title = "Home", .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_HIDDEN, .enter = enter, .leave = leave,
    .draw = draw, .key = key, .tick = tick, .tick_ms = HOME_TICK_MS, .obscured = obscured, .exposed = exposed,
    .legends = HOME_LEGENDS, .nlegends = 2,
};
