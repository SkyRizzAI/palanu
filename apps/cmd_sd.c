/* sd: the memory card (pal/sd.h). The slot has no card-detect line, so a card put in after boot is mounted here or in
 * Settings > Storage; eject before pulling it out. */
#include <stdio.h>
#include <string.h>

#include "libapp.h"
#include "pal/sd.h"

static void status(void) {
    if (!pal_sd_present()) {
        pal_puts("sd: this board has no card slot\n");
        return;
    }
    if (!pal_sd_mounted()) {
        pal_puts("sd: no card mounted (sd mount looks for one)\n");
        return;
    }
    pal_sd_info_t in = {0};
    uint64_t total = 0, free_bytes = 0;
    bool info = pal_sd_info(&in), space = pal_sd_space(&total, &free_bytes);
    pal_printf("sd: card at %s", PAL_SD_PATH);
    if (info) pal_printf(", %s, %lu MB, %lu MHz", in.name[0] ? in.name : "card", (unsigned long)(in.size_bytes >> 20),
                         (unsigned long)(in.speed_khz / 1000u));
    pal_puts("\n");
    if (space)
        pal_printf("    %lu MB free of %lu MB\n", (unsigned long)(free_bytes >> 20), (unsigned long)(total >> 20));
}

static int cmd_sd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        status();
        return 0;
    }
    if (strcmp(argv[1], "mount") == 0) {
        nema_err_t r = pal_sd_mount();
        if (r != NEMA_OK) {
            pal_eprintf("sd: %s\n", r == NEMA_ERR_NOSYS ? "no card slot" : r == NEMA_ERR_NOENT ? "no card found" : pal_strerror(r));
            return 1;
        }
        status();
        return 0;
    }
    if (strcmp(argv[1], "eject") == 0) {
        (void)pal_sd_eject();
        pal_puts("sd: the card can be removed\n");
        return 0;
    }
    pal_eprintf("usage: sd [status | mount | eject]\n");
    return 1;
}

static const nema_app_t SD_APP = {"sd", "memory card: status, mount, eject", cmd_sd, 8192, 0};

void pal_apps_register_sd(void);
void pal_apps_register_sd(void) { nema_app_register(&SD_APP); }
