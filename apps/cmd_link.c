/* `link`: Palanu Link's pairing, paired clients, and the cable trust setting (docs/PLP.md, "Pairing"). `link pair` opens
 * the 120 s pairing window and prints the code for the computer (`palanu pair`). `link revoke` removes a client and
 * ends its live session. The shell is a trusted channel, so these commands run from the cable or an authenticated link. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/link.h"

static void usage(void) { pal_eprintf("usage: link [status | pair | list | revoke ID|all | trust uart on|off]\n"); }

static int cmd_link(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        char win[96], clients[512];
        pal_link_pair_status(win, sizeof(win));
        pal_printf("cable (UART): %s\n", pal_link_uart_trusted() ? "trusted (link.uart.trust=on)"
                                                                  : "untrusted until a client authenticates");
        pal_printf("wireless (TCP, BLE): untrusted until a client authenticates\n");
        pal_printf("connected: %s\n", pal_link_connected() ? "yes" : "no");
        pal_printf("%s\n", win);
        size_t n = pal_link_clients_text(clients, sizeof(clients));
        if (n) pal_printf("paired clients:\n%s", clients);
        else pal_printf("paired clients: none\n");
        return 0;
    }
    if (strcmp(argv[1], "pair") == 0) {
        char code[PAL_LINK_CODE_LEN + 1];
        if (pal_link_pair_open(code) != NEMA_OK) {
            pal_eprintf("link: cannot open a pairing window\n");
            return 1;
        }
        pal_printf("pairing code: %s\nenter it on the computer within 120 s, 3 tries: palanu pair --tcp HOST:PORT\n", code);
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        char clients[512];
        size_t n = pal_link_clients_text(clients, sizeof(clients));
        pal_printf("%s", n ? clients : "no paired clients\n");
        return 0;
    }
    if (strcmp(argv[1], "revoke") == 0 && argc == 3) {
        nema_err_t r = pal_link_revoke(argv[2]);
        if (r == NEMA_ERR_INVAL) pal_eprintf("link: an id is 16 hex digits (see `link list`), or all\n");
        else if (r == NEMA_ERR_NOENT) pal_eprintf("link: no paired client %s\n", argv[2]);
        else if (r != NEMA_OK) pal_eprintf("link: revoke failed (%d)\n", (int)r);
        else pal_printf("revoked %s\n", argv[2]);
        return r == NEMA_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "trust") == 0 && argc == 4 && strcmp(argv[2], "uart") == 0) {
        bool on = strcmp(argv[3], "on") == 0;
        if (!on && strcmp(argv[3], "off") != 0) {
            usage();
            return 2;
        }
        if (pal_link_uart_trust(on) != NEMA_OK) {
            pal_eprintf("link: cannot save the setting\n");
            return 1;
        }
        pal_printf("link.uart.trust=%s%s\n", on ? "on" : "off", on ? "" : " (the cable needs a token now)");
        return 0;
    }
    usage();
    return 2;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t LINK_APPS[] = {
    APP("link", "Palanu Link: status, pair, list, revoke, cable trust", cmd_link, 8192),
};

void pal_apps_register_link(void);
void pal_apps_register_link(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(LINK_APPS); i++) nema_app_register(&LINK_APPS[i]);
}
