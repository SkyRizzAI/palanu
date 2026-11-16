/* The uidemo shell command: opens the dialog, text input, toast and app switcher so the golden images can show them. It
 * is a test aid, not a user command: the registry hides the command, and nothing else calls it. Each dialog reports its answer
 * as a toast, so a person at the device sees the callback ran. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/dialog.h"
#include "pal/overlay.h"
#include "pal/toast.h"

static void on_confirm(bool ok, void *ctx) {
    (void)ctx;
    psys_toast(ok ? "Confirmed" : "Cancelled", 1500);
}

static void on_input(bool ok, const char *text, void *ctx) {
    (void)ctx;
    char msg[48];
    if (ok) snprintf(msg, sizeof(msg), "Entered %u characters", (unsigned)strlen(text));
    else snprintf(msg, sizeof(msg), "Input cancelled");
    psys_toast(msg, 1500);
}

static int cmd_uidemo(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "dialog") == 0) {
        pal_dialog_confirm("Erase data", "Erase every file in /home? This cannot be undone.", "Erase", "Keep", on_confirm, NULL);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "input") == 0) {
        pal_textinput_open("Wi-Fi password", "", 20, true, on_input, NULL);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "toast") == 0) {
        psys_toast("Saved to /home/notes.txt", 60000);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "switcher") == 0) {
        pal_desktop_switcher_show();
        return 0;
    }
    pal_eprintf("usage: uidemo dialog | input | toast | switcher\n");
    return 1;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t UIDEMO_APPS[] = {
    APP("uidemo", "test aid: open a dialog, text input, toast or the app switcher", cmd_uidemo, 0),
};

void pal_apps_register_uidemo(void);
void pal_apps_register_uidemo(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(UIDEMO_APPS); i++) nema_app_register(&UIDEMO_APPS[i]);
}
