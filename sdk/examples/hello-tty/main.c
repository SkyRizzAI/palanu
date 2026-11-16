/* hello-tty: a terminal app with a menu (paltui, docs/TUI.md). Up/Down move, Enter chooses, Esc quits.
 * Build: bun forge/tools/palanu.ts build   (in this directory; writes hello-tty.papp) */
#define PALANU_LIBC_IMPLEMENTATION
#include "palanu_libc.h"
#define PALTUI_IMPLEMENTATION
#define PALTUI_WITH_PAL
#include "paltui/paltui.h"

static tui_t ui; /* paltui keeps its state in this struct; it is too big for the stack */
static const char *const items[] = {"Say hello", "Show uptime", "Quit"};
#define NITEMS ((int)(sizeof(items) / sizeof(items[0])))

void _start(void) {
    int sel = 0, cols = 80, rows = 24, quit = 0;
    char msg[64] = "Choose with Up/Down and Enter.";
    if (tui_open(&ui, tui_io_pal()) != 0) return;
    while (!quit && tui_frame(&ui, 1000) == TUI_RUN) {
        tui_size(&ui, &cols, &rows);
        tui_region(&ui, 2, 1, cols - 4, rows - 2);
        tui_title(&ui, "Hello, PalanuOS");
        tui_gap(&ui, 1);
        int chose = tui_menu(&ui, "items", items, NITEMS, &sel);
        tui_gap(&ui, 1);
        tui_label(&ui, msg);
        tui_status(&ui, " Up/Down move   Enter choose   Esc quit");
        const tui_key_t *k = tui_key(&ui);
        if (k && (k->sym == TUI_K_ESC || (k->mods & TUI_MOD_CTRL && k->cp == 'c'))) quit = 1;
        tui_end(&ui);
        if (!chose || quit) continue;
        if (sel == 0) snprintf(msg, sizeof msg, "Hello from WebAssembly.");
        else if (sel == 1) snprintf(msg, sizeof msg, "Uptime: %lld ms", (long long)pal_now_ms());
        else quit = 1;
    }
    tui_close(&ui);
    printf("hello-tty: bye\n");
}
