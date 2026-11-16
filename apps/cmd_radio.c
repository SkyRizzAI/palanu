/* Radio commands: radio (every radio, memory), and ble on | off | status. Wi-Fi is in cmd_net.c (wifi on | off).
 * Policy and the numbers behind it: docs/RADIO.md. */
#include <stdio.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/sysinfo.h"

static void holders_text(const pal_radio_info_t *in, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < PAL_RADIO_HOLDERS; i++) {
        if (!in->holders[i][0]) continue;
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", n ? ", " : "", in->holders[i]);
    }
    if (!out[0]) snprintf(out, cap, "-");
}

static void print_radio(pal_radio_t r) {
    pal_radio_info_t in;
    pal_radio_info(r, &in);
    char holders[PAL_RADIO_HOLDERS * PAL_RADIO_HOLDER_LEN + 8], detail[128];
    holders_text(&in, holders, sizeof(holders));
    pal_radio_detail(r, detail, sizeof(detail));
    pal_printf("%-5s %-9s holders %-24s cost %luK%s\n", pal_radio_name(r), pal_radio_state_name(in.state), holders,
               (unsigned long)pal_radio_cost_kb(r), in.grace_pending ? "  (stops after the grace period)" : "");
    if (detail[0]) pal_printf("      %s\n", detail);
    if (in.error[0]) pal_printf("      last error: %s\n", in.error);
}

static int cmd_radio(int argc, char **argv) {
    (void)argc;
    (void)argv;
    print_radio(PAL_RADIO_WIFI);
    print_radio(PAL_RADIO_BLE);
    size_t free_b = 0, min_b = 0, largest = 0;
    pal_sys_mem_stats(PAL_MEM_INTERNAL, &free_b, &min_b, &largest);
    pal_printf("internal RAM: free %luK, min %luK, largest block %luK\n", (unsigned long)(free_b / 1024),
               (unsigned long)(min_b / 1024), (unsigned long)(largest / 1024));
    return 0;
}

static int cmd_ble(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        print_radio(PAL_RADIO_BLE);
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        /* persisted even when refused, so the next boot tries again once memory allows */
        pal_settings_set_radio("radio.ble", "on");
        (void)psys_svc_start("radio-ble"); /* the service holds the radio; a refusal retries every 5 s */
        if (pal_radio_state(PAL_RADIO_BLE) != PAL_RADIO_ON) {
            pal_eprintf("ble: %s\n", pal_radio_last_error(PAL_RADIO_BLE));
            return 1;
        }
        pal_printf("ble: on, advertising as PalanuOS\n");
        return 0;
    }
    if (strcmp(argv[1], "off") == 0) {
        (void)psys_svc_stop("link-ble"); /* the link holds the radio too */
        (void)psys_svc_stop("radio-ble"); /* releases the radio and records the choice (radio.ble=off) */
        pal_printf("ble: off%s\n", pal_radio_state(PAL_RADIO_BLE) == PAL_RADIO_ON ? " (stops after the grace period)" : "");
        return 0;
    }
    pal_eprintf("usage: ble [on | off | status]\n");
    return 1;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t RADIO_APPS[] = {
    APP("radio", "radios: state, holders, cost, memory", cmd_radio, 8192),
    APP("ble", "Bluetooth LE link: on, off, status", cmd_ble, 8192),
};

void pal_apps_register_radio(void);
void pal_apps_register_radio(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(RADIO_APPS); i++) nema_app_register(&RADIO_APPS[i]);
}
