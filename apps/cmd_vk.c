/* vkeyboard: the on-screen keyboard from the shell. Scenes and tests use it (docs/VKEYBOARD.md): "vkeyboard open TEXT LAYER"
 * shows fixed text, "vkeyboard entry TITLE [mask]" runs the text-entry screen and prints the result. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/vk.h"

static int usage(void) {
    pal_eprintf("usage: vkeyboard [open TEXT [abc|ABC|123|#+=|ctl] | close | entry TITLE [mask|-] [TEXT]]\n");
    return 1;
}

static int cmd_vkeyboard(int argc, char **argv) {
    if (argc < 2) return pal_vk_open() == NEMA_OK ? 0 : usage();
    if (strcmp(argv[1], "close") == 0) {
        pal_vk_close();
        return 0;
    }
    if (strcmp(argv[1], "open") == 0) {
        if (argc < 3) return pal_vk_open() == NEMA_OK ? 0 : 1;
        return pal_vk_open_text(argv[2], argc > 3 ? argv[3] : "abc") == NEMA_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "entry") == 0) {
        if (argc < 3) return usage();
        char buf[128];
        /* entry TITLE [mask|-] [TEXT]: TEXT is what the box starts with (scenes show a prefilled name) */
        const char *initial = argc > 4 ? argv[4] : "";
        pal_vk_entry_t e = {argv[2], initial, buf, sizeof(buf), argc > 3 && strcmp(argv[3], "mask") == 0, true};
        int r = pal_vk_edit(&e);
        if (r == PAL_VK_CANCEL) {
            pal_printf("cancelled\n");
            return 1;
        }
        if (r < 0) {
            pal_eprintf("vkeyboard: no keyboard (%d)\n", r);
            return 1;
        }
        pal_printf("entered %d bytes\n", r);
        /* scenes and tests read the text back; a user does not see it printed */
        if (getenv("PALANU_VK_TEST")) pal_printf("[vk:%s]\n", buf);
        return 0;
    }
    return usage();
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t VK_APPS[] = {
    APP("vkeyboard", "on-screen keyboard: open, entry", cmd_vkeyboard, 8192),
};

void pal_apps_register_vk(void);
void pal_apps_register_vk(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(VK_APPS); i++) nema_app_register(&VK_APPS[i]);
}
