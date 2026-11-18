/* A canvas app (API level 6, api/palanu/ui.h): RGB565 pixels that look the same under every desktop. A colour gradient and a square
 * the arrow keys move; Esc ends it. Build: bun forge/tools/palanu.ts build sdk/examples/canvas-demo.c --id canvas-demo
 * --name "Canvas demo" --kind gui --api 6 --mem 256 -o canvas-demo.papp */
#include "../palanu.h"

#define MAXW 320
static uint8_t row[MAXW * 2];

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void paint(int w, int h, int sx, int sy) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t c = rgb(x * 255 / w, y * 255 / h, 160);
            if (x >= sx && x < sx + 24 && y >= sy && y < sy + 24) c = 0xFFFF;
            row[2 * x] = (uint8_t)c, row[2 * x + 1] = (uint8_t)(c >> 8);
        }
        pal_canvas_present(row, (uint32_t)w * 2u, 0, y, w);
    }
}

void _start(void) {
    int32_t size = pal_canvas_open("Canvas demo");
    if (size < 0) pal_exit(1);
    int w = size >> 16, h = size & 0xFFFF, x = w / 2 - 12, y = h / 2 - 12;
    if (w > MAXW) w = MAXW;
    paint(w, h, x, y);
    for (;;) {
        uint8_t k[8];
        int r = pal_canvas_key(k, sizeof(k), -1);
        if (r == 2) break; /* the person left the screen */
        if (r != 1) continue;
        if (k[0] == PAL_SYM_ESC) break;
        if (k[0] == PAL_SYM_LEFT && x > 0) x -= 8;
        if (k[0] == PAL_SYM_RIGHT && x < w - 24) x += 8;
        if (k[0] == PAL_SYM_UP && y > 0) y -= 8;
        if (k[0] == PAL_SYM_DOWN && y < h - 24) y += 8;
        paint(w, h, x, y);
    }
    pal_canvas_close();
}
