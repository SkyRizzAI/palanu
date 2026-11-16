/* anim: the animation library from the shell (docs/ANIM.md). `anim list` names every animation (built-in and in /data/anims),
 * `anim info NAME` says what a file holds and what it costs, `anim play NAME [scale]` opens it full screen (ENTER triggers its active
 * sequence, Esc closes), `anim sheet NAME [page]` shows its frames, and `anim bench NAME [scale]` times the drawing on this board.
 * Hidden from the launcher (dev), like uishow. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "nema/mem.h"
#include "pal/anim.h"
#include "pal/gfx.h"
#include "pal/sysinfo.h"
#include "pal/ui.h"

static const char USAGE[] =
    "usage: anim list | info NAME | play NAME [1-3] | sheet NAME [PAGE] | bench NAME [1-3]\n";

static pal_anim_t *open_any(const char *name) {
    pal_anim_t *a = pal_anim_open_builtin(name);
    return a ? a : pal_anim_open_file(name);
}

static int cmd_list(void) {
    char names[32][PAL_ANIM_NAME_MAX];
    int n = pal_anim_list(names, 32);
    if (n == 0) pal_printf("no animations\n");
    for (int i = 0; i < n; i++) {
        pal_anim_t *a = open_any(names[i]);
        const pal_anim_info_t *inf = a ? pal_anim_info(a) : NULL;
        if (inf)
            pal_printf("%-16s %3dx%-3d %2d fps  %2d frames  %3d positions  %s\n", names[i], inf->width, inf->height, inf->fps,
                       inf->frames, inf->positions, inf->indexed ? "colour (v2)" : "mono");
        else
            pal_printf("%-16s (does not open)\n", names[i]);
        pal_anim_close(a);
    }
    return 0;
}

static int cmd_info(const char *name) {
    pal_anim_t *a = open_any(name);
    if (!a) {
        pal_eprintf("anim: no animation %s\n", name);
        return 1;
    }
    const pal_anim_info_t *i = pal_anim_info(a);
    pal_printf("name        %s\n", pal_anim_name(a));
    pal_printf("format      .panim v%d, %s\n", i->version, i->indexed ? "indexed4 (colour)" : "mono1");
    pal_printf("size        %dx%d, %d fps\n", i->width, i->height, i->fps);
    pal_printf("bitmaps     %d unique, %d playback positions\n", i->frames, i->positions);
    pal_printf("playback    passive %d, active %d\n", i->passive, i->active);
    pal_printf("bytes       %lu file, %lu held in memory (PSRAM for a file, flash for a built-in)\n", (unsigned long)i->bytes,
               (unsigned long)pal_anim_memory(a));
    pal_anim_close(a);
    return 0;
}

static int cmd_play(const char *name, int scale) {
    pal_anim_t *a = open_any(name);
    if (!a) {
        pal_eprintf("anim: no animation %s\n", name);
        return 1;
    }
    pal_anim_close(a);
    pal_ui_anim_play(name, scale);
    pal_printf("anim: playing %s at %dx (Esc closes)\n", name, scale);
    return 0;
}

static int cmd_sheet(const char *name, int page) {
    pal_anim_t *a = open_any(name);
    if (!a) {
        pal_eprintf("anim: no animation %s\n", name);
        return 1;
    }
    pal_anim_close(a);
    pal_ui_anim_sheet(name, page);
    return 0;
}

/* Times the drawing of every frame at the scale, fitted to the 320x240 screen, into a PSRAM canvas. */
static int cmd_bench(const char *name, int scale) {
    pal_anim_t *a = open_any(name);
    if (!a) {
        pal_eprintf("anim: no animation %s\n", name);
        return 1;
    }
    const pal_anim_info_t *inf = pal_anim_info(a);
    size_t bytes = 320u * 240u;
    uint8_t *px = nema_alloc(NEMA_MEM_EXTERNAL, nema_owner_register("anim"), bytes);
    if (!px) {
        pal_anim_close(a);
        pal_eprintf("anim: no memory for the bench canvas\n");
        return 1;
    }
    pal_canvas_t c;
    pal_canvas_init(&c, px, 320, 240);
    pal_clear(&c, PAL_C_PAPER);
    int reps = 20, draws = 0;
    pal_rect_t area = scale > 0 ? (pal_rect_t){0, 0, inf->width * scale, inf->height * scale}
                                : pal_anim_fit_rect(inf->width, inf->height, (pal_rect_t){0, 0, 320, 240}, PAL_ANIM_FIT_FIT,
                                                    PAL_ANCHOR_CENTER);
    uint64_t t0 = pal_sys_uptime_us();
    for (int r = 0; r < reps; r++)
        for (int f = 0; f < inf->frames; f++, draws++) {
            if (scale > 0) pal_anim_draw(&c, a, f, 0, 0, scale, PAL_C_INK, PAL_TRANSPARENT);
            else pal_anim_draw_fit(&c, a, f, (pal_rect_t){0, 0, 320, 240}, PAL_ANIM_FIT_FIT, PAL_ANCHOR_CENTER, PAL_C_INK, PAL_C_PAPER);
        }
    uint64_t dt = pal_sys_uptime_us() - t0;
    pal_printf("bench %s %s: %d draws, %lu us each\n", name, scale > 0 ? "at scale" : "fit 320x240", draws,
               (unsigned long)(dt / (uint64_t)(draws ? draws : 1)));
    pal_printf("the frame's rectangle is %dx%d px: %d bytes sent per frame at 16 bit\n", area.w, area.h, area.w * area.h * 2);
    nema_free(px);
    pal_anim_close(a);
    return 0;
}

static int cmd_anim(int argc, char **argv) {
    if (argc < 2) {
        pal_eprintf("%s", USAGE);
        return 1;
    }
    const char *op = argv[1];
    if (!strcmp(op, "list")) return cmd_list();
    if (argc < 3) {
        pal_eprintf("%s", USAGE);
        return 1;
    }
    int n = argc > 3 ? atoi(argv[3]) : 0;
    if (!strcmp(op, "info")) return cmd_info(argv[2]);
    if (!strcmp(op, "play")) return cmd_play(argv[2], n >= 1 && n <= 3 ? n : 2);
    if (!strcmp(op, "sheet")) return cmd_sheet(argv[2], n);
    if (!strcmp(op, "bench")) return cmd_bench(argv[2], n >= 1 && n <= 3 ? n : 0);
    pal_eprintf("%s", USAGE);
    return 1;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t ANIM_APPS[] = {
    APP("anim", "the animation library: list, info, play, sheet (frames), bench (dev)", cmd_anim, 12288),
};

void pal_apps_register_anim(void);
void pal_apps_register_anim(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(ANIM_APPS); i++) nema_app_register(&ANIM_APPS[i]);
}
