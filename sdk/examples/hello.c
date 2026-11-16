/* Prints its arguments and the time since boot. */
#include "../palanu.h"

void _start(void) {
    pal_puts("hello from WebAssembly on PalanuOS\n");
    int n = pal_argc();
    for (int i = 1; i < n; i++) {
        char a[64];
        int len = pal_argv(i, a, sizeof(a));
        if (len < 0) continue;
        pal_puts("  arg: ");
        pal_write(1, a, (unsigned)len);
        pal_puts("\n");
    }
    pal_puts("uptime ms: ");
    pal_putu((unsigned long long)pal_now_ms());
    pal_puts("\n");
}
