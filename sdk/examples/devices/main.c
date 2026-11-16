/* Lists every device the board declares, then lights each LED red, green and blue in turn and plays a tone on each buzzer.
 * It runs unchanged on a board with one LED and on a board with five: it asks the board what it has. */
#include "../../palanu.h"

static const uint32_t COLORS[] = {0xff0000, 0x00ff00, 0x0000ff, 0x000000};

/* pal_dev_info gives "id<TAB>kind<TAB>name" (a led adds "<TAB>pixels<TAB>colors"): the kind starts after the first tab. */
static char kind_of(const char *info, int n) {
    for (int i = 0; i < n; i++)
        if (info[i] == '\t') return info[i + 1];
    return 0;
}

void _start(void) {
    int total = pal_dev_total();
    pal_puts("devices: ");
    pal_putu((unsigned long long)total);
    pal_puts("\n");
    for (int i = 0; i < total; i++) {
        char info[96];
        int n = pal_dev_info(i, info, sizeof(info));
        if (n < 0) continue;
        pal_puts("  ");
        pal_puts(info);
        pal_puts("\n");
        char kind = kind_of(info, n);
        if (kind == 'l') { /* led: every pixel in each colour, then off */
            for (unsigned c = 0; c < 4; c++) {
                pal_dev_set_led(i, -1, COLORS[c]);
                pal_sleep_ms(300);
            }
        } else if (kind == 'b') { /* buzzer */
            pal_dev_play_tone(i, 880, 150);
            pal_sleep_ms(300);
        }
    }
}
