/* kui palettes: the reference colour themes. The reference
 * RGB565 values are written here as 0xRRGGBB, the form PalanuOS themes use; the sim and panel expand them with the same
 * rounding, so the pixels match the reference exactly. palanu is the default; dark mode swaps ink and paper. */
#include <string.h>

#include "pal/kui.h"

static const kui_palette_t PALANU = {"palanu", 0x292442, 0xAD9EEF};
static const kui_palette_t MONO = {"mono", 0x000000, 0xFFFFFF};
static const kui_palette_t ORANGE = {"orange", 0x000000, 0xFF6500};

const kui_palette_t *kui_palette_named(const char *name) {
    if (name && strcmp(name, "mono") == 0) return &MONO;
    if (name && strcmp(name, "orange") == 0) return &ORANGE;
    return &PALANU;
}

/* Dark mode keeps the palette name and swaps its two colours. */
const kui_palette_t *kui_palette_dark(const kui_palette_t *p) {
    static kui_palette_t dark[3];
    kui_palette_t *d = NULL;
    if (p == &MONO) d = &dark[0];
    else if (p == &ORANGE) d = &dark[1];
    else { d = &dark[2]; p = &PALANU; }
    d->name = p->name;
    d->ink = p->paper;
    d->paper = p->ink;
    return d;
}
