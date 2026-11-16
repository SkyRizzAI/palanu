/* `note text...` appends a line to /home/notes.txt (needs the "fs.home" permission: `--perm fs.home`); `note` alone
 * prints them. Without the permission the file cannot be opened. */
#include "../palanu.h"

#define PATH "/home/notes.txt"

void _start(void) {
    if (pal_argc() < 2) {
        int fd = pal_open(PATH, sizeof(PATH) - 1, PAL_O_READ);
        if (fd < 0) { pal_puts("no notes yet\n"); return; }
        char b[128];
        int n;
        while ((n = pal_read(fd, b, sizeof(b))) > 0) pal_write(1, b, (unsigned)n);
        pal_close(fd);
        return;
    }
    int fd = pal_open(PATH, sizeof(PATH) - 1, PAL_O_WRITE | PAL_O_CREATE | PAL_O_APPEND);
    if (fd < 0) { pal_puts("cannot open the notes file\n"); pal_exit(1); }
    for (int i = 1; i < pal_argc(); i++) {
        char a[96];
        int len = pal_argv(i, a, sizeof(a));
        if (len < 0) continue;
        pal_write(fd, a, (unsigned)len);
        pal_write(fd, i + 1 < pal_argc() ? " " : "\n", 1);
    }
    pal_close(fd);
}
