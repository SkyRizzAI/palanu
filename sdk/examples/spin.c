/* Never returns: for testing the instruction limit (the manifest's fuel) that stops runaway apps. */
#include "../palanu.h"

void _start(void) {
    volatile unsigned long x = 0;
    for (;;) x++;
}
