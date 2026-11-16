/* The animation internals shared by anim.c (parsing, loading, listing) and anim_draw.c (playback and drawing). Not API. */
#ifndef SYSTEM_ANIM_INT_H
#define SYSTEM_ANIM_INT_H

#include "pal/anim.h"

#define ANIM_MAX_PAL 15

/* What the parser learns from the bytes. Offsets are into the file's bytes. */
typedef struct {
    pal_anim_info_t info;
    size_t frame0;   /* offset of bitmap 0 */
    size_t bpf;      /* bytes per bitmap */
    size_t order_off; /* offset of framesOrder, valid when info.positions came from it */
    bool has_order;  /* false: sequential playback (playhead i is bitmap i) */
    uint8_t npal;
    uint16_t pal_rgb[ANIM_MAX_PAL];
    uint8_t pal_mo[ANIM_MAX_PAL];
} anim_parsed_t;

struct pal_anim {
    char name[PAL_ANIM_NAME_MAX];
    anim_parsed_t p;
    const uint8_t *data; /* the .panim bytes: flash for built-ins, the owned PSRAM copy for files */
    void *owned;         /* the block to free (the struct itself, with the file bytes after it); NULL for a borrowed blob */
    size_t block_bytes;  /* the size of that block: what the animation holds in memory */
};

/* Checks the bytes as a .panim (both versions) and fills p. */
nema_err_t anim_parse(const uint8_t *d, size_t len, anim_parsed_t *p);
/* The bitmap index at a playhead position. */
int anim_bitmap_at(const pal_anim_t *a, int ph);

#endif
