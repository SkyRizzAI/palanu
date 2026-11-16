/* kui status bar: the reference StatusBar::draw for the inverted theme
 * (palanu, mono, orange all use it): the bar is filled with ink and its clock, battery and Wi-Fi are drawn in paper.
 * The reference BLE, SD and lock icons are PalanuOS status items here (statusitems.c), drawn after this bar. */
#include "pal/kui.h"

/* 3x5 digits, rows top to bottom, 3 bits each, MSB leftmost (the reference kTinyDigit). */
static const uint8_t TINY_DIGIT[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7}, {0x7, 0x1, 0x7, 0x1, 0x7},
    {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7}, {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x2, 0x2, 0x2},
    {0x7, 0x5, 0x7, 0x5, 0x7}, {0x7, 0x5, 0x7, 0x1, 0x7},
};

static int tiny_digit(kui_canvas_t *c, int x, int y, int d, bool on) {
    const uint8_t *g = TINY_DIGIT[d % 10];
    for (int r = 0; r < 5; r++)
        for (int col = 0; col < 3; col++)
            if (g[r] & (1 << (2 - col))) kui_pixel(c, x + col, y + r, on);
    return x + 4; /* 3 wide and a gap */
}

/* The reference StatusBar::drawClock: HH, a 1 px colon, MM. */
static void tiny_clock(kui_canvas_t *c, int x, int y, int h, int m, bool on) {
    x = tiny_digit(c, x, y, h / 10, on);
    x = tiny_digit(c, x, y, h % 10, on);
    kui_pixel(c, x, y + 1, on);
    kui_pixel(c, x, y + 3, on);
    x += 2;
    x = tiny_digit(c, x, y, m / 10, on);
    tiny_digit(c, x, y, m % 10, on);
}

/* The reference drawBattery: a 10x6 outline, a 1x2 nub, and a charge bar of pct% of the 8 inner pixels. */
static void battery(kui_canvas_t *c, int x, int y, int pct, bool on) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    kui_frame(c, x, y, 10, 6, on);
    kui_fill(c, x + 10, y + 2, 1, 2, on);
    int fw = (pct * 8) / 100;
    if (fw > 0) kui_fill(c, x + 1, y + 1, fw, 4, on);
}

/* The reference StatusBar::drawWifi: four 1 px bars of heights 2..5, bottom aligned, 7x5. */
static void wifi(kui_canvas_t *c, int x, int y, bool on) {
    static const uint8_t bh[4] = {2, 3, 4, 5};
    for (int i = 0; i < 4; i++) kui_fill(c, x + i * 2, y + 5 - bh[i], 1, bh[i], on);
}

void kui_status_draw(kui_canvas_t *c, const kui_status_t *st) {
    const int barH = KUI_STATUS_H;
    kui_fill(c, 0, 0, c->w, barH, true); /* inverted bar: filled with ink */
    const bool on = false;               /* text and icons are paper on the filled bar */
    tiny_clock(c, 2, (barH - 5) / 2, st->hour, st->minute, on);

    int rx = c->w - 2;
    if (st->battery >= 0) {
        rx = rx > 13 ? rx - 13 : 0;             /* kBatW (11) + 2 */
        battery(c, rx, (barH - 6) / 2, st->battery, on);
    }
    if (st->wifi) {
        rx = rx > 9 ? rx - 9 : 0;               /* kWifiW (7) + 2 */
        wifi(c, rx, (barH - 5) / 2, on);
    }
}
