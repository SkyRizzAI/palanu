/* PalanuOS animations: the .panim player (docs/ANIM.md). An animation is a list of 1-bit or 4-bit bitmaps and a playback order.
 * The player runs a passive loop, and an optional active sequence when it is triggered; the frame clock is driven by the caller
 * (a view's tick), so there is no thread here. Built-in animations are embedded (anims_data.c); others are files under
 * /data/anims. Both are read in place (embedded) or copied into PSRAM (files). Drawing goes through the palette slots, so
 * dark mode and the other themes recolour an animation with no change to it. */
#ifndef PAL_ANIM_H
#define PAL_ANIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"
#include "pal/gfx.h"

#define PAL_ANIM_MAX_BYTES (512u * 1024u) /* a file larger than this is refused */
#define PAL_ANIM_MAX_W 320
#define PAL_ANIM_MAX_H 240
#define PAL_ANIM_NAME_MAX 24              /* a name: a-z, 0-9, '-' (the file name without .panim) */
#define PAL_ANIM_DIR "/data/anims"

/* How an animation is fitted into a rectangle (the same four modes as the reference wallpaper). */
typedef enum {
    PAL_ANIM_FIT_CENTER, /* native size, placed by the anchor */
    PAL_ANIM_FIT_STRETCH, /* scaled to the rectangle, aspect ignored */
    PAL_ANIM_FIT_CROP,   /* uniform scale that covers the rectangle; the overflow is clipped */
    PAL_ANIM_FIT_FIT,    /* uniform scale that fits inside; the rest is left alone (letterbox) */
} pal_anim_fit_t;

/* The nine anchors, row by row (the same order as the reference Anchor). */
typedef enum {
    PAL_ANCHOR_TOP_LEFT,
    PAL_ANCHOR_TOP,
    PAL_ANCHOR_TOP_RIGHT,
    PAL_ANCHOR_LEFT,
    PAL_ANCHOR_CENTER,
    PAL_ANCHOR_RIGHT,
    PAL_ANCHOR_BOTTOM_LEFT,
    PAL_ANCHOR_BOTTOM,
    PAL_ANCHOR_BOTTOM_RIGHT,
} pal_anchor_t;

/* What a file says about itself (pure: from the bytes alone). */
typedef struct {
    int version;      /* 1 (mono) or 2 (mono or indexed4) */
    int width, height;
    int fps;
    int frames;       /* unique bitmaps */
    int positions;    /* playback positions (the order length, or the frame count when sequential) */
    int passive;      /* positions in the idle loop */
    int active;       /* positions in the triggered sequence */
    bool indexed;     /* v2 with pixelFormat indexed4 */
    size_t bytes;     /* the file size */
} pal_anim_info_t;

typedef struct pal_anim pal_anim_t;

/* Reads the header and checks every table and frame against the size. Returns NEMA_OK, or NEMA_ERR_INVAL for a file that is
 * malformed, of an unknown version, or larger than the screen. Does not allocate. */
nema_err_t pal_anim_parse(const uint8_t *data, size_t len, pal_anim_info_t *out);

/* Opens an embedded animation (read in place from flash) or a file under PAL_ANIM_DIR (copied into PSRAM, owner "anim"). NULL when
 * it does not exist or does not parse. Close with pal_anim_close. */
pal_anim_t *pal_anim_open_builtin(const char *name);
pal_anim_t *pal_anim_open_file(const char *name);
/* Opens a .panim held in memory. The bytes are read in place and must outlive the animation. */
pal_anim_t *pal_anim_open_blob(const char *name, const uint8_t *data, size_t len);
void pal_anim_close(pal_anim_t *a);
/* The bytes the animation holds: the file in PSRAM plus its struct, or only the struct for a built-in (its frames stay in flash). */
size_t pal_anim_memory(const pal_anim_t *a);
const pal_anim_info_t *pal_anim_info(const pal_anim_t *a);
const char *pal_anim_name(const pal_anim_t *a);

/* Every animation the system has: the built-in ones first, then the files in PAL_ANIM_DIR, without repeats. Writes up to max
 * names (each at most PAL_ANIM_NAME_MAX bytes including the NUL) and returns the count. */
int pal_anim_list(char names[][PAL_ANIM_NAME_MAX], int max);
bool pal_anim_exists(const char *name);

/* ---- playback. The caller passes the time (ms) on every tick; nothing runs by itself. */
typedef struct {
    const pal_anim_t *a;
    int playhead;   /* 0 .. positions-1 */
    bool active;    /* the triggered sequence is playing */
    bool started;
    uint32_t last_ms;
} pal_anim_player_t;

void pal_anim_play_start(pal_anim_player_t *p, const pal_anim_t *a, uint32_t now_ms);
/* Plays the active sequence once, then the passive loop again. No effect when the animation has none. */
void pal_anim_play_trigger(pal_anim_player_t *p);
/* Moves the playhead to position ph (clamped). For frozen screens (the golden images). */
void pal_anim_play_seek(pal_anim_player_t *p, int ph);
/* Advances by the elapsed time at the animation's frame rate. Returns true when the bitmap on screen changed. */
bool pal_anim_play_tick(pal_anim_player_t *p, uint32_t now_ms);
/* The bitmap index at the playhead. */
int pal_anim_play_bitmap(const pal_anim_player_t *p);

/* ---- drawing. on: colour of mono "on" pixels (PAL_TRANSPARENT skips them); off: colour of the rest (PAL_TRANSPARENT leaves the
 * canvas as it is). Indexed frames use their palette: see pal_anim_slot. Each call marks only the rectangle it drew. */
/* Draws bitmap bm at (x, y) with an integer scale (1, 2 or 3; nearest neighbour). Clipped to the canvas clip. */
void pal_anim_draw(pal_canvas_t *c, const pal_anim_t *a, int bm, int x, int y, int scale, pal_color_t on, pal_color_t off);
/* The rectangle a w x h animation occupies when fitted into area (may be larger than area for crop; clip it). */
pal_rect_t pal_anim_fit_rect(int w, int h, pal_rect_t area, pal_anim_fit_t fit, pal_anchor_t anchor);
/* Draws bitmap bm fitted into area. off fills the whole area first when it is not PAL_TRANSPARENT. */
void pal_anim_draw_fit(pal_canvas_t *c, const pal_anim_t *a, int bm, pal_rect_t area, pal_anim_fit_t fit, pal_anchor_t anchor,
                       pal_color_t on, pal_color_t off);

/* The mono (1-bit) bits of bitmap bm, MSB first and row by row, for the kui canvas: bit set = ink. A Mono1 frame is returned in
 * place; an indexed4 frame is converted into buf (at least ceil(w*h/8) bytes, else NULL), ink where the palette slot is ink. */
const uint8_t *pal_anim_mono_bits(const pal_anim_t *a, int bm, uint8_t *buf, size_t cap);

/* Maps a v2 palette entry to a theme slot: monoOverride 1 is ink and 2 is paper; auto (0) is accent when the colour is
 * saturated, otherwise ink, dim, panel or paper by luminance. rgb565 is the entry's colour. */
pal_color_t pal_anim_slot(uint16_t rgb565, uint8_t mono_override);

#endif
