/* The animation screens (docs/ANIM.md, docs/UI.md): the player (anim play NAME [scale]), a full-screen view of one animation that
 * plays its passive loop; ENTER triggers the active sequence and Esc closes it (Esc goes home, as for any front view). And the
 * contact sheet (anim sheet NAME [page]): every frame of an animation, six to a page, for reviewing the art and the golden images.
 * Both open through pal_desktop_open, so the shell can start them. */
#include <stdio.h>
#include <string.h>

#include "pal/anim.h"
#include "pal/desktop.h"
#include "pal/display.h"
#include "pal/sysinfo.h"
#include "pal/theme.h"
#include "pal/ui.h"

#define SHEET_PER_PAGE 6
#define ANIM_TICK_MS 100

static struct {
    char name[PAL_ANIM_NAME_MAX]; /* the animation to open (set by pal_ui_anim_play / pal_ui_anim_sheet) */
    int scale;
    int page;
    pal_anim_t *anim;
    pal_anim_player_t player;
} V = {.scale = 1};

static pal_anim_t *open_named(const char *name) {
    pal_anim_t *a = pal_anim_open_builtin(name);
    return a ? a : pal_anim_open_file(name);
}

void pal_ui_anim_play(const char *name, int scale) {
    nema_strlcpy(V.name, name ? name : "", sizeof(V.name));
    V.scale = scale < 1 ? 1 : scale > 3 ? 3 : scale;
    pal_desktop_open("animplay");
}

void pal_ui_anim_sheet(const char *name, int page) {
    nema_strlcpy(V.name, name ? name : "", sizeof(V.name));
    V.page = page < 0 ? 0 : page;
    pal_desktop_open("animsheet");
}

/* ---- the player ---- */

/* The largest scale not above the wanted one that fits the area (the legend bar is outside it). */
static int fit_scale(const pal_anim_info_t *inf, int want, pal_rect_t a) {
    int s = want;
    while (s > 1 && (inf->width * s > a.w || inf->height * s > a.h)) s--;
    return s;
}

static void play_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_PAPER);
    if (!V.anim) {
        const char *msg = "No such animation";
        pal_text(c, t->tokens.body, (a.w - pal_text_w(t->tokens.body, msg)) / 2, a.h / 2, msg, PAL_C_INK, PAL_TRANSPARENT);
    } else {
        const pal_anim_info_t *inf = pal_anim_info(V.anim);
        int s = fit_scale(inf, V.scale, a);
        int x = a.x + (a.w - inf->width * s) / 2, y = a.y + (a.h - inf->height * s) / 2;
        pal_anim_draw(c, V.anim, pal_anim_play_bitmap(&V.player), x, y, s, PAL_C_INK, PAL_TRANSPARENT);
    }
    if (V.anim) {
        const pal_anim_info_t *inf = pal_anim_info(V.anim);
        char cap[48];
        snprintf(cap, sizeof(cap), "%s  %dx%d  %d fps  %d frames", pal_anim_name(V.anim), inf->width, inf->height, inf->fps, inf->frames);
        pal_text(c, t->tokens.caption, a.x + 4, a.y + 2, cap, PAL_C_DIM, PAL_TRANSPARENT);
    }
}

static void play_enter(pal_view_t *v) {
    (void)v;
    if (V.anim) pal_anim_close(V.anim);
    V.anim = open_named(V.name);
    pal_anim_play_start(&V.player, V.anim, 0);
}

static void play_leave(pal_view_t *v) {
    (void)v;
    if (V.anim) pal_anim_close(V.anim);
    V.anim = NULL;
}

static void play_tick(pal_view_t *v, uint32_t now) {
    (void)v;
    if (!V.anim) return;
    if (pal_anim_play_tick(&V.player, now)) {
        pal_canvas_t *c = pal_display_canvas();
        pal_rect_t a = pal_desktop_area(&pal_animplay_view);
        const pal_anim_info_t *inf = pal_anim_info(V.anim);
        int s = fit_scale(inf, V.scale, a);
        int x = a.x + (a.w - inf->width * s) / 2, y = a.y + (a.h - inf->height * s) / 2;
        pal_clip_reset(c);
        pal_fill(c, x, y, inf->width * s, inf->height * s, PAL_C_PAPER);
        pal_anim_draw(c, V.anim, pal_anim_play_bitmap(&V.player), x, y, s, PAL_C_INK, PAL_TRANSPARENT);
    }
}

static bool play_key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (k->sym == PAL_KEY_ENTER && V.anim) {
        pal_anim_play_trigger(&V.player);
        return true;
    }
    return false;
}

static const pal_legend_item_t PLAY_LEGENDS[] = {{PAL_ACT_OK, "active", NULL}, {PAL_ACT_BACK, "close", NULL}};

pal_view_t pal_animplay_view = {
    .id = "animplay", .title = "Animation", .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_HIDDEN, .enter = play_enter,
    .leave = play_leave, .draw = play_draw, .key = play_key, .tick = play_tick, .tick_ms = ANIM_TICK_MS,
    .legends = PLAY_LEGENDS, .nlegends = 2,
};

/* ---- the contact sheet: two columns of frames, three rows, each with its number ---- */

static void sheet_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    const pal_font_t *cf = t->tokens.caption;
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_PAPER);
    if (!V.anim) {
        const char *msg = "No such animation";
        pal_text(c, t->tokens.body, (a.w - pal_text_w(t->tokens.body, msg)) / 2, a.h / 2, msg, PAL_C_INK, PAL_TRANSPARENT);
        return;
    }
    const pal_anim_info_t *inf = pal_anim_info(V.anim);
    int first = V.page * SHEET_PER_PAGE;
    for (int k = 0; k < SHEET_PER_PAGE; k++) {
        int idx = first + k;
        if (idx >= inf->frames) break;
        int x = 24 + (k % 2) * 144, y = 6 + (k / 2) * 74;
        pal_anim_draw(c, V.anim, idx, x, y, 1, PAL_C_INK, PAL_TRANSPARENT);
        pal_rect(c, x - 1, y - 1, inf->width + 2, inf->height + 2, PAL_C_DIM);
        char lab[24];
        snprintf(lab, sizeof(lab), "frame %d", idx);
        pal_text(c, cf, x, y + inf->height + 3, lab, PAL_C_DIM, PAL_TRANSPARENT);
    }
    int pages = (inf->frames + SHEET_PER_PAGE - 1) / SHEET_PER_PAGE;
    char foot[64];
    snprintf(foot, sizeof(foot), "%s  page %d of %d  (%d frames, %d fps)", pal_anim_name(V.anim), V.page + 1, pages,
             inf->frames, inf->fps);
    pal_text(c, cf, 8, a.h - cf->h - 2, foot, PAL_C_INK, PAL_TRANSPARENT);
}

static void sheet_enter(pal_view_t *v) {
    (void)v;
    if (V.anim) pal_anim_close(V.anim);
    V.anim = open_named(V.name);
    if (V.anim) {
        int pages = (pal_anim_info(V.anim)->frames + SHEET_PER_PAGE - 1) / SHEET_PER_PAGE;
        if (V.page >= pages) V.page = 0;
    }
}

static bool sheet_key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (!V.anim) return false;
    int pages = (pal_anim_info(V.anim)->frames + SHEET_PER_PAGE - 1) / SHEET_PER_PAGE;
    if (k->sym == PAL_KEY_DOWN || k->sym == PAL_KEY_ENTER) {
        V.page = (V.page + 1) % pages;
        pal_desktop_invalidate_all();
        return true;
    }
    if (k->sym == PAL_KEY_UP) {
        V.page = (V.page + pages - 1) % pages;
        pal_desktop_invalidate_all();
        return true;
    }
    return false;
}

pal_view_t pal_animsheet_view = {
    .id = "animsheet", .title = "Frames", .flags = PAL_VIEW_FULLSCREEN | PAL_VIEW_HIDDEN, .enter = sheet_enter,
    .leave = play_leave, .draw = sheet_draw, .key = sheet_key,
};
