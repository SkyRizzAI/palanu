/* A host picker with paltui: a menu, a prompt for a new host, a confirm before "connecting", and a status bar.
 * Build: bun forge/tools/palanu.ts build sdk/examples/menu.c --id menu -o /tmp/menu.papp   (adds -I sdk itself)
 * The picked host is printed after the screen is restored. Keys: Up/Down, Enter, Esc or Ctrl-C to quit. On the device the
 * same keys are also shown in the legend bar under the terminal (pal_legend_show); the system removes them when it ends. */
#define PALTUI_IMPLEMENTATION
#define PALTUI_WITH_PAL
#include "paltui/paltui.h"

static tui_t ui;
static const char *const hosts[] = {"home-nas", "build-box", "lab-pi", "add a new host..."};
#define NHOSTS ((int)(sizeof(hosts) / sizeof(hosts[0])))

static void say(const char *s, unsigned n) { pal_write(1, s, n); }

void _start(void) {
    char picked[32] = "";
    char typed[32] = "";
    int sel = 0, cols = 80, rows = 24, quit = 0;
    if (tui_open(&ui, tui_io_pal()) != 0) return;
    pal_legend_show("ok=Choose;back=Quit;move_v=Move"); /* most useful first: what does not fit is dropped from the right */
    while (!quit && tui_frame(&ui, 1000) == TUI_RUN) {
        tui_size(&ui, &cols, &rows);
        tui_region(&ui, 2, 1, cols - 4, rows - 2);
        tui_title(&ui, "Pick a host");
        tui_gap(&ui, 1);
        int chose = tui_menu(&ui, "hosts", hosts, NHOSTS, &sel);
        tui_gap(&ui, 1);
        tui_label(&ui, picked[0] ? "last picked:" : "nothing picked yet");
        if (picked[0]) tui_label(&ui, picked);
        tui_status(&ui, " Up/Down move   Enter choose   Esc quit");
        const tui_key_t *k = tui_key(&ui); /* keys no widget took */
        if (k && (k->sym == TUI_K_ESC || (k->mods & TUI_MOD_CTRL && k->cp == 'c'))) quit = 1;
        tui_end(&ui);
        if (!chose || quit) continue;
        /* modals run between frames, over the screen just shown */
        if (sel == NHOSTS - 1) {
            typed[0] = 0;
            if (!tui_prompt(&ui, "New host name", typed, sizeof(typed)) || !typed[0]) continue;
            for (unsigned i = 0; i < sizeof(picked); i++) picked[i] = typed[i];
        } else {
            unsigned i = 0;
            for (; hosts[sel][i] && i + 1 < sizeof(picked); i++) picked[i] = hosts[sel][i];
            picked[i] = 0;
        }
        if (tui_confirm(&ui, "Connect now? (this example only prints the choice)")) quit = 2;
        else picked[0] = 0;
    }
    tui_close(&ui);
    if (quit == 2) {
        say("picked: ", 8);
        pal_write(1, picked, pal_strlen(picked));
        say("\n", 1);
    }
}
