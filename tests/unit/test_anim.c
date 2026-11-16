/* The animation library (docs/ANIM.md, system/src/anim*.c): the .panim reader against the reference's own bytes, the bounds and malformed
 * files, the player on a fake clock, the fit and anchor maths, scaling and the dirty rectangle, the palette slots, the file and
 * built-in lists, and the desktop wallpaper repainted by part. No display: the canvas is a plain buffer. */
#include <string.h>

#include "nema/nema.h"
#include "nema/vfs.h"
#include "pal/anim.h"
#include "pal/gfx.h"
#include "pal/theme.h"
#include "pal/settings.h"
#include "pal/ui.h"
#include "test.h"
#include "anim_fixtures.h"
#include "pal/kui.h"

#define W 320
#define H 240
static uint8_t PX[W * H];

static pal_canvas_t canvas_with(uint8_t fill) {
    pal_canvas_t c;
    pal_canvas_init(&c, PX, W, H);
    pal_fill(&c, 0, 0, W, H, fill);
    pal_take_dirty(&c, &(pal_rect_t){0});
    return c;
}

/* ---- the reader ---- */
TEST(reads_legacy_v1_mono_exactly) {
    pal_anim_info_t i;
    CHECK_EQ(pal_anim_parse(LEGACY_V1, sizeof(LEGACY_V1), &i), NEMA_OK);
    CHECK_EQ(i.version, 1);
    CHECK_EQ(i.width, 12);
    CHECK_EQ(i.height, 4);
    CHECK_EQ(i.fps, 8);
    CHECK_EQ(i.frames, 3);
    CHECK_EQ(i.positions, 5);
    CHECK_EQ(i.passive, 3);
    CHECK_EQ(i.active, 2);
    CHECK(!i.indexed);
}

TEST(reads_legacy_v2_indexed_palette) {
    pal_anim_info_t i;
    CHECK_EQ(pal_anim_parse(LEGACY_V2, sizeof(LEGACY_V2), &i), NEMA_OK);
    CHECK_EQ(i.version, 2);
    CHECK(i.indexed);
    CHECK_EQ(i.width, 5);
    CHECK_EQ(i.frames, 2);
    CHECK_EQ(i.passive, 2);
    CHECK_EQ(i.active, 0);
}

TEST(rejects_bad_magic_versions_and_truncation) {
    uint8_t b[sizeof(LEGACY_V1)];
    memcpy(b, LEGACY_V1, sizeof(b));
    b[0] = 'X';
    CHECK(pal_anim_parse(b, sizeof(b), NULL) == NEMA_ERR_INVAL);
    memcpy(b, LEGACY_V1, sizeof(b));
    b[4] = 3; /* an unknown version */
    CHECK(pal_anim_parse(b, sizeof(b), NULL) == NEMA_ERR_INVAL);
    CHECK(pal_anim_parse(LEGACY_V1, 10, NULL) == NEMA_ERR_INVAL);                       /* short header */
    CHECK(pal_anim_parse(LEGACY_V1, sizeof(LEGACY_V1) - 1, NULL) == NEMA_ERR_INVAL);     /* short frame data */
    CHECK(pal_anim_parse(LEGACY_V2, 15, NULL) == NEMA_ERR_INVAL);                       /* short v2 header */
    CHECK(pal_anim_parse(NULL, 0, NULL) == NEMA_ERR_INVAL);
}

TEST(rejects_bad_tables_and_oversize_dimensions) {
    uint8_t b[sizeof(LEGACY_V1)];
    memcpy(b, LEGACY_V1, sizeof(b));
    b[14] = 9; /* the first playback entry points past the bitmaps */
    CHECK(pal_anim_parse(b, sizeof(b), NULL) == NEMA_ERR_INVAL);
    memcpy(b, LEGACY_V1, sizeof(b));
    b[10] = 6; /* passive count beyond the playback positions */
    CHECK(pal_anim_parse(b, sizeof(b), NULL) == NEMA_ERR_INVAL);
    memcpy(b, LEGACY_V2, sizeof(b));
    b[16] = 0; /* palette count 0 */
    CHECK(pal_anim_parse(b, sizeof(b), NULL) == NEMA_ERR_INVAL);
    /* width 321 is past the screen; the rest of the header is valid (one bitmap, no passive or active run) */
    uint8_t big[14 + 41];
    memset(big, 0, sizeof(big));
    memcpy(big, LEGACY_V1, 14);
    big[5] = 65; /* 321 = 0x141 */
    big[6] = 1;
    big[7] = 1;
    big[8] = 0;
    big[10] = 0;
    big[11] = 0;
    big[12] = 1;
    big[13] = 0;
    CHECK(pal_anim_parse(big, sizeof(big), NULL) == NEMA_ERR_INVAL);
    big[5] = 64; /* 320 is the largest width: one row of 40 bytes is 40 bytes of bitmap, and the file has them */
    big[6] = 1;
    CHECK_EQ(pal_anim_parse(big, sizeof(big), NULL), NEMA_OK);
}

/* ---- the built-in animations and the lists ---- */
TEST(built_in_animations_open_and_are_sane) {
    static const char *const NAMES[] = {"coder", "sleepy-cat", "server", "rain", "rocket", "coffee"};
    for (size_t k = 0; k < sizeof(NAMES) / sizeof(NAMES[0]); k++) {
        pal_anim_t *a = pal_anim_open_builtin(NAMES[k]);
        CHECK(a != NULL);
        if (!a) continue;
        const pal_anim_info_t *i = pal_anim_info(a);
        CHECK_EQ(i->width, 128);
        CHECK_EQ(i->height, 64);
        CHECK(i->frames >= 1 && i->frames <= 24);
        CHECK(i->fps >= 1 && i->fps <= 8);
        CHECK(i->positions >= 1 && i->positions <= 255);
        CHECK(pal_anim_exists(NAMES[k]));
        CHECK(pal_anim_memory(a) < 512); /* a built-in holds only its struct: the frames stay in flash */
        pal_anim_close(a);
    }
    CHECK(pal_anim_open_builtin("no-such") == NULL);
    CHECK(pal_anim_open_builtin("../coder") == NULL);
    CHECK(pal_anim_open_builtin("Coder") == NULL);
    CHECK(!pal_anim_exists("Bad Name"));
    CHECK(!pal_anim_exists(""));
}

TEST(file_animations_load_into_psram_and_bad_ones_are_refused) {
    nema_fs_mkdir("/data/anims");
    nema_obj_t *f;
    CHECK_EQ(nema_fs_open("/data/anims/legacy.panim", NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f), NEMA_OK);
    nema_obj_write(f, LEGACY_V1, sizeof(LEGACY_V1), 0);
    nema_obj_unref(f);
    pal_anim_t *a = pal_anim_open_file("legacy");
    CHECK(a != NULL);
    if (a) {
        CHECK_EQ(pal_anim_info(a)->width, 12);
        CHECK(pal_anim_memory(a) >= sizeof(LEGACY_V1)); /* the bytes are copied */
        CHECK(strcmp(pal_anim_name(a), "legacy") == 0);
        pal_anim_close(a);
    }
    CHECK_EQ(nema_fs_open("/data/anims/broken.panim", NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f), NEMA_OK);
    nema_obj_write(f, LEGACY_V1, 30, 0); /* truncated */
    nema_obj_unref(f);
    CHECK(pal_anim_open_file("broken") == NULL);
    CHECK(pal_anim_exists("legacy"));
    CHECK(!pal_anim_exists("missing"));
}

TEST(oversize_files_are_refused_before_they_are_read) {
    nema_fs_mkdir("/data/anims");
    static uint8_t big[PAL_ANIM_MAX_BYTES + 1];
    memset(big, 0, sizeof(big));
    memcpy(big, LEGACY_V1, sizeof(LEGACY_V1));
    nema_obj_t *f;
    CHECK_EQ(nema_fs_open("/data/anims/huge.panim", NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &f), NEMA_OK);
    nema_obj_write(f, big, sizeof(big), 0);
    nema_obj_unref(f);
    CHECK(pal_anim_open_file("huge") == NULL);
}

TEST(list_names_built_ins_then_files_without_repeats) {
    char names[24][PAL_ANIM_NAME_MAX];
    int n = pal_anim_list(names, 24);
    CHECK(n >= 7); /* six built-in and the legacy file written above */
    int builtins_first = strcmp(names[0], "coder") == 0;
    CHECK(builtins_first);
    int legacy = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], "legacy") == 0) legacy++;
        for (int j = 0; j < i; j++) CHECK(strcmp(names[i], names[j]) != 0);
    }
    CHECK_EQ(legacy, 1);
    CHECK_EQ(pal_anim_list(names, 2), 2);
}

/* ---- playback on a fake clock: LEGACY_V1 at 8 fps is one step every 125 ms; passive positions 0..2 show bitmaps 0,1,1; active
 * positions 3..4 show 2,0 ---- */
TEST(passive_loop_then_active_sequence_on_a_fake_clock) {
    pal_anim_t *a = pal_anim_open_blob("legacy", LEGACY_V1, sizeof(LEGACY_V1));
    CHECK(a != NULL);
    pal_anim_player_t p;
    pal_anim_play_start(&p, a, 1000);
    CHECK(!pal_anim_play_tick(&p, 1000));          /* the first tick starts the clock */
    CHECK(!pal_anim_play_tick(&p, 1124));          /* not yet */
    CHECK(pal_anim_play_tick(&p, 1125));           /* position 1: bitmap 1 */
    CHECK_EQ(p.playhead, 1);
    CHECK_EQ(pal_anim_play_bitmap(&p), 1);
    CHECK(!pal_anim_play_tick(&p, 1250));          /* position 2 is bitmap 1 as well: nothing new on screen */
    CHECK_EQ(p.playhead, 2);
    CHECK(pal_anim_play_tick(&p, 1375));           /* the passive loop wraps to position 0 */
    CHECK_EQ(p.playhead, 0);
    CHECK_EQ(pal_anim_play_bitmap(&p), 0);
    pal_anim_play_trigger(&p);                     /* the active sequence starts at position 3 */
    CHECK(p.active);
    CHECK_EQ(p.playhead, 3);
    CHECK(pal_anim_play_tick(&p, 1500));           /* position 4 (the second active position): bitmap 0 */
    CHECK_EQ(p.playhead, 4);
    CHECK_EQ(pal_anim_play_bitmap(&p), 0);
    CHECK(!pal_anim_play_tick(&p, 1600));          /* 100 ms: no step yet */
    CHECK(!pal_anim_play_tick(&p, 1625));          /* the active run ends at its last position: back to the passive loop at 0 */
    CHECK(!p.active);
    CHECK_EQ(p.playhead, 0);
    CHECK(pal_anim_play_tick(&p, 1750));           /* the passive loop moves on to position 1 */
    CHECK(!p.active);
    CHECK_EQ(p.playhead, 1);
    pal_anim_play_seek(&p, 99);                    /* clamped to the last position */
    CHECK_EQ(p.playhead, 4);
    CHECK(!p.active);
    /* a long gap runs several positions in one tick: 250 ms is two steps of 125 ms, 0 -> 1 -> 2 */
    pal_anim_play_start(&p, a, 0);
    pal_anim_play_tick(&p, 0);
    CHECK(pal_anim_play_tick(&p, 250));
    CHECK_EQ(p.playhead, 2);
    CHECK_EQ(pal_anim_play_bitmap(&p), 1);
    pal_anim_close(a);
}

TEST(trigger_without_an_active_sequence_does_nothing) {
    pal_anim_t *a = pal_anim_open_blob("legacy2", LEGACY_V2, sizeof(LEGACY_V2));
    CHECK(a != NULL);
    pal_anim_player_t p;
    pal_anim_play_start(&p, a, 0);
    pal_anim_play_trigger(&p);
    CHECK(!p.active);
    CHECK_EQ(p.playhead, 0);
    pal_anim_close(a);
    /* the clock wraps around the 32-bit millisecond counter */
    pal_anim_t *b = pal_anim_open_blob("legacy", LEGACY_V1, sizeof(LEGACY_V1));
    pal_anim_play_start(&p, b, 0xFFFFFF00u);
    pal_anim_play_tick(&p, 0xFFFFFF00u);
    CHECK(pal_anim_play_tick(&p, 0x00000000u)); /* 256 ms later (across the wrap): two steps */
    CHECK_EQ(p.playhead, 2);
    pal_anim_close(b);
}

/* ---- fit and anchor maths ---- */
static int same(pal_rect_t a, int x, int y, int w, int h) { return a.x == x && a.y == y && a.w == w && a.h == h; }

TEST(fit_rect_for_each_mode_and_anchor) {
    pal_rect_t area = {0, 0, 320, 240};
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_CENTER, PAL_ANCHOR_TOP_LEFT), 0, 0, 128, 64));
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_CENTER, PAL_ANCHOR_CENTER), 96, 88, 128, 64));
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_CENTER, PAL_ANCHOR_BOTTOM_RIGHT), 192, 176, 128, 64));
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_FIT, PAL_ANCHOR_CENTER), 0, 40, 320, 160));
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_FIT, PAL_ANCHOR_TOP), 0, 0, 320, 160));
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_STRETCH, PAL_ANCHOR_CENTER), 0, 0, 320, 240));
    /* crop covers the screen: 480 x 240, overflowing left and right; the anchor picks the side that stays */
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_CROP, PAL_ANCHOR_CENTER), -80, 0, 480, 240));
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_CROP, PAL_ANCHOR_TOP_LEFT), 0, 0, 480, 240));
    CHECK(same(pal_anim_fit_rect(128, 64, area, PAL_ANIM_FIT_CROP, PAL_ANCHOR_BOTTOM_RIGHT), -160, 0, 480, 240));
    /* a tall picture fits by height */
    CHECK(same(pal_anim_fit_rect(16, 64, area, PAL_ANIM_FIT_FIT, PAL_ANCHOR_CENTER), 130, 0, 60, 240));
    /* a fit into an area that does not start at the origin */
    CHECK(same(pal_anim_fit_rect(128, 64, (pal_rect_t){10, 20, 100, 100}, PAL_ANIM_FIT_FIT, PAL_ANCHOR_CENTER), 10, 45, 100, 50));
    /* the anchors run in reading order: index 4 is the centre */
    CHECK_EQ(PAL_ANCHOR_CENTER, 4);
    CHECK_EQ(PAL_ANCHOR_BOTTOM_RIGHT, 8);
}

/* ---- drawing: scale, the mapping of mono and indexed frames, and the dirty rectangle ---- */
TEST(integer_scale_doubles_every_pixel) {
    pal_anim_t *a = pal_anim_open_blob("legacy", LEGACY_V1, sizeof(LEGACY_V1));
    pal_canvas_t c = canvas_with(7);
    /* frame 0 row 0 is 1111 0000: columns 0..3 are on, 4..7 off. Rows are contiguous bits (bit = row * width + col), so row 1 starts
     * in the middle of byte 1 at bit 4, which is on */
    pal_anim_draw(&c, a, 0, 10, 10, 2, PAL_C_INK, PAL_TRANSPARENT);
    CHECK_EQ(pal_get(&c, 10, 10), PAL_C_INK);
    CHECK_EQ(pal_get(&c, 11, 11), PAL_C_INK);
    CHECK_EQ(pal_get(&c, 17, 10), PAL_C_INK);  /* column 3, the last on column */
    CHECK_EQ(pal_get(&c, 18, 10), 7);          /* column 4 is off: transparent, the background stays */
    CHECK_EQ(pal_get(&c, 10, 12), PAL_C_INK);  /* source (0, 1): bit 12 */
    CHECK_EQ(pal_get(&c, 18, 12), PAL_C_INK);  /* source (4, 1): bit 16, the top bit of byte 2 */
    pal_anim_close(a);
}

TEST(draw_marks_only_the_rectangle_it_covered) {
    pal_anim_t *a = pal_anim_open_blob("legacy", LEGACY_V1, sizeof(LEGACY_V1));
    pal_canvas_t c = canvas_with(0);
    pal_rect_t d = {0};
    pal_take_dirty(&c, &d);
    pal_anim_draw(&c, a, 0, 50, 60, 2, PAL_C_INK, PAL_TRANSPARENT);
    CHECK(pal_take_dirty(&c, &d));
    CHECK(same(d, 50, 60, 24, 8));
    /* 12x4 fitted into 40x40 is 40x13, centred vertically at y 113; a transparent off colour marks only that */
    pal_anim_draw_fit(&c, a, 1, (pal_rect_t){100, 100, 40, 40}, PAL_ANIM_FIT_FIT, PAL_ANCHOR_CENTER, PAL_C_INK, PAL_TRANSPARENT);
    CHECK(pal_take_dirty(&c, &d));
    CHECK(same(d, 100, 113, 40, 13));
    CHECK_EQ(pal_get(&c, 100, 100), 0); /* the canvas outside the drawn rectangle is untouched */
    /* a paper off colour repaints the whole area first, so the dirty rectangle is the area */
    pal_anim_draw_fit(&c, a, 1, (pal_rect_t){100, 100, 40, 40}, PAL_ANIM_FIT_FIT, PAL_ANCHOR_CENTER, PAL_C_INK, PAL_C_PAPER);
    CHECK(pal_take_dirty(&c, &d));
    CHECK(same(d, 100, 100, 40, 40));
    CHECK_EQ(pal_get(&c, 100, 100), PAL_C_PAPER);
    pal_anim_close(a);
}

TEST(indexed_frames_use_their_palette_and_clamp_unknown_entries) {
    pal_anim_t *a = pal_anim_open_blob("legacy2", LEGACY_V2, sizeof(LEGACY_V2));
    pal_canvas_t c = canvas_with(7);
    /* frame 0, row 0 bytes 0x12 0x30 0x00: values 1, 2, 3, 0, 0; value 1 is red (auto: accent), 2 is white (force ink), 3 is past the
     * palette and stays transparent */
    pal_anim_draw(&c, a, 0, 0, 0, 1, PAL_C_INK, PAL_TRANSPARENT);
    CHECK_EQ(pal_get(&c, 0, 0), PAL_C_ACCENT);
    CHECK_EQ(pal_get(&c, 1, 0), PAL_C_INK);
    CHECK_EQ(pal_get(&c, 2, 0), 7);
    CHECK_EQ(pal_get(&c, 3, 0), 7);
    pal_anim_close(a);
}

TEST(palette_entries_map_to_theme_slots) {
    CHECK_EQ(pal_anim_slot(0xFFFF, 0), PAL_C_PAPER);   /* white */
    CHECK_EQ(pal_anim_slot(0x0000, 0), PAL_C_INK);     /* black */
    CHECK_EQ(pal_anim_slot(0xF800, 0), PAL_C_ACCENT);  /* saturated red */
    CHECK_EQ(pal_anim_slot(0x8410, 0), PAL_C_DIM);     /* mid grey */
    CHECK_EQ(pal_anim_slot(0xE71C, 0), PAL_C_PANEL);   /* light grey */
    CHECK_EQ(pal_anim_slot(0xF800, 1), PAL_C_INK);     /* force ink */
    CHECK_EQ(pal_anim_slot(0x0000, 2), PAL_C_PAPER);   /* force paper */
}

/* ---- the desktop wallpaper (kui_wall_fit): a repaint of part of the screen equals a full paint ---- */
TEST(desktop_wallpaper_repaints_by_part) {
    pal_anim_t *a = pal_anim_open_builtin("laptop-cat");
    CHECK(a != NULL);
    if (!a) return;
    const pal_anim_info_t *inf = pal_anim_info(a);
    static uint8_t mono[PAL_ANIM_MAX_W * PAL_ANIM_MAX_H / 8];
    static kui_canvas_t full, parts;
    for (int bm = 0; bm < 3; bm++) {
        const uint8_t *bits = pal_anim_mono_bits(a, bm, mono, sizeof(mono));
        CHECK(bits != NULL);
        /* the content area of the desktop: y 11..120 of the 160 x 120 logical screen, fitted and centred */
        kui_canvas_init(&full, 320, 240, 8);
        kui_wall_fit(&full, bits, inf->width, inf->height, 0, 11, 160, 109, KUI_FIT_FIT, 4);
        kui_canvas_init(&parts, 320, 240, 8);
        /* the same wallpaper drawn in two clipped parts (a repaint of part of the screen) */
        kui_clip(&parts, 0, 11, 160, 50);
        kui_wall_fit(&parts, bits, inf->width, inf->height, 0, 11, 160, 109, KUI_FIT_FIT, 4);
        kui_clip(&parts, 0, 61, 160, 59);
        kui_wall_fit(&parts, bits, inf->width, inf->height, 0, 11, 160, 109, KUI_FIT_FIT, 4);
        kui_clip_reset(&parts);
        CHECK(memcmp(full.bits, parts.bits, sizeof(full.bits)) == 0);
    }
    pal_anim_close(a);
}

/* ---- the settings rows (settings_api.c) for the animation screens ---- */
static int get_is(const char *key, const char *want) {
    char b[48];
    return psys_settings_get(key, b, sizeof(b)) >= 0 && strcmp(b, want) == 0;
}

TEST(settings_rows_for_the_animation_screens) {
    /* the lock animation: "none" or an animation name, validated against the library */
    CHECK_EQ(psys_settings_set("ui.lock_anim", "none"), NEMA_OK);
    CHECK(get_is("ui.lock_anim", "none"));
    CHECK_EQ(psys_settings_set("ui.lock_anim", "rocket"), NEMA_OK);
    CHECK(get_is("ui.lock_anim", "rocket"));
    CHECK(psys_settings_set("ui.lock_anim", "no-such") != NEMA_OK);
    CHECK(get_is("ui.lock_anim", "rocket")); /* a refused value changes nothing */
    /* ui.wallpaper: the desktop animations by name (PAL_UI_WALL_NAMES); anim:NAME of one of them is accepted; old values load as the first */
    CHECK_EQ(psys_settings_set("ui.wallpaper", "desk-hacker"), NEMA_OK);
    CHECK(get_is("ui.wallpaper", "desk-hacker"));
    CHECK(pal_ui_wallpaper() == PAL_UI_WALL_ANIM);
    CHECK_EQ(psys_settings_set("ui.wallpaper", "lab"), NEMA_OK);
    CHECK(get_is("ui.wallpaper", "lab"));
    CHECK_EQ(psys_settings_set("ui.wallpaper", "anim:laptop-cat"), NEMA_OK);
    CHECK(get_is("ui.wallpaper", "laptop-cat"));
    CHECK(psys_settings_set("ui.wallpaper", "anim:nope") != NEMA_OK);
    CHECK(psys_settings_set("ui.wallpaper", "rain") != NEMA_OK);
    CHECK_EQ(psys_settings_set("ui.wallpaper", "waves"), NEMA_OK); /* an old value: maps to the default */
    CHECK(get_is("ui.wallpaper", "laptop-cat"));
    /* fit and anchor: the names, in their order */
    CHECK_EQ(psys_settings_set("ui.wall_fit", "crop"), NEMA_OK);
    CHECK(get_is("ui.wall_fit", "crop"));
    CHECK(psys_settings_set("ui.wall_fit", "zoom") != NEMA_OK);
    CHECK_EQ(psys_settings_set("ui.wall_anchor", "top-left"), NEMA_OK);
    CHECK(get_is("ui.wall_anchor", "top-left"));
    CHECK_EQ(psys_settings_set("ui.wall_anchor", "bottom-right"), NEMA_OK);
    CHECK(get_is("ui.wall_anchor", "bottom-right"));
    CHECK(pal_ui_wall_anchor() == PAL_ANCHOR_BOTTOM_RIGHT);
    /* restore the defaults for the other tests and screens */
    psys_settings_set("ui.wall_fit", "fit");
    psys_settings_set("ui.wall_anchor", "bottom");
    psys_settings_set("ui.wallpaper", "laptop-cat");
    psys_settings_set("ui.lock_anim", "sleepy-cat");
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    nema_vfs_mount("/", nema_ramfs_ops(), nema_ramfs_create(1200u * 1024u)); /* a 512 KB file and the prefs fit */
    nema_fs_mkdir("/data");
    RUN(reads_legacy_v1_mono_exactly);
    RUN(reads_legacy_v2_indexed_palette);
    RUN(rejects_bad_magic_versions_and_truncation);
    RUN(rejects_bad_tables_and_oversize_dimensions);
    RUN(built_in_animations_open_and_are_sane);
    RUN(file_animations_load_into_psram_and_bad_ones_are_refused);
    RUN(oversize_files_are_refused_before_they_are_read);
    RUN(list_names_built_ins_then_files_without_repeats);
    RUN(passive_loop_then_active_sequence_on_a_fake_clock);
    RUN(trigger_without_an_active_sequence_does_nothing);
    RUN(fit_rect_for_each_mode_and_anchor);
    RUN(integer_scale_doubles_every_pixel);
    RUN(draw_marks_only_the_rectangle_it_covered);
    RUN(indexed_frames_use_their_palette_and_clamp_unknown_entries);
    RUN(palette_entries_map_to_theme_slots);
    RUN(desktop_wallpaper_repaints_by_part);
    RUN(settings_rows_for_the_animation_screens);
    return TEST_DONE();
}
