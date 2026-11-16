/* Counts lines, words and bytes on stdin: `echo a b c | count`. Shows that an app is an ordinary process. */
#include "../palanu.h"

void _start(void) {
    char buf[256];
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0, n;
    while ((n = pal_read(0, buf, sizeof(buf))) > 0) {
        bytes += (unsigned long)n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            int sp = c == ' ' || c == '\n' || c == '\t' || c == '\r';
            if (!sp && !in_word) words++;
            in_word = !sp;
        }
    }
    pal_putu(lines), pal_puts(" "), pal_putu(words), pal_puts(" "), pal_putu(bytes), pal_puts("\n");
}
