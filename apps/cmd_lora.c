/* lora: the board's LoRa module (pal/lora.h, ADR-0020). info shows the module and its settings; set changes them (they are kept);
 * send sends a line; listen prints what arrives, with its RSSI and SNR, until Ctrl-C or the seconds given. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/lora.h"
#include "pal/settings.h"
#include "pal/sysinfo.h"

static void show(void) {
    const pal_device_t *d = pal_lora_device();
    pal_lora_cfg_t c;
    pal_lora_cfg_load(&c);
    pal_printf("module    %s\n", d ? d->name : "none on this board");
    pal_printf("frequency %lu.%03lu MHz\n", (unsigned long)(c.freq_hz / 1000000u), (unsigned long)(c.freq_hz / 1000u % 1000u));
    pal_printf("spreading SF%d, bandwidth %d kHz, coding 4/%d, power %d dBm\n", c.sf, c.bw_khz, c.cr, c.power_dbm);
    pal_printf("on air    %lu ms for 32 bytes\n", (unsigned long)pal_lora_airtime_ms(&c, 32));
}

static const char *why(int e) {
    switch (e) {
    case NEMA_ERR_NOENT: return "no LoRa module on this board";
    case NEMA_ERR_INVAL: return "the settings are not valid (lora info)";
    case NEMA_ERR_TIMEOUT: return "the module did not finish in time";
    case NEMA_ERR_PROTO: return "a packet with a bad CRC";
    default: return pal_strerror(e);
    }
}

/* set freq 923.2 sf 9 bw 125 cr 5 power 14: checked together, then kept */
static int cmd_set(int argc, char **argv) {
    pal_lora_cfg_t c;
    pal_lora_cfg_load(&c);
    for (int i = 2; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "freq")) {
            double mhz = atof(v);
            c.freq_hz = mhz > 10000 ? (uint32_t)mhz : (uint32_t)(mhz * 1000000.0 + 0.5); /* MHz, or Hz */
        } else if (!strcmp(k, "sf")) c.sf = (uint8_t)atoi(v);
        else if (!strcmp(k, "bw")) c.bw_khz = (uint16_t)atoi(v);
        else if (!strcmp(k, "cr")) c.cr = (uint8_t)atoi(v);
        else if (!strcmp(k, "power")) c.power_dbm = (int8_t)atoi(v);
        else {
            pal_eprintf("lora: unknown setting %s (freq, sf, bw, cr, power)\n", k);
            return 2;
        }
    }
    const char *bad = pal_lora_cfg_check(&c);
    if (bad) {
        pal_eprintf("lora: %s\n", bad);
        return 1;
    }
    char t[16];
    snprintf(t, sizeof(t), "%lu", (unsigned long)c.freq_hz);
    psys_settings_set("lora.freq", t);
    snprintf(t, sizeof(t), "%d", c.sf);
    psys_settings_set("lora.sf", t);
    snprintf(t, sizeof(t), "%d", c.bw_khz);
    psys_settings_set("lora.bw", t);
    snprintf(t, sizeof(t), "%d", c.cr);
    psys_settings_set("lora.cr", t);
    snprintf(t, sizeof(t), "%d", c.power_dbm);
    psys_settings_set("lora.power", t);
    pal_settings_save();
    show();
    return 0;
}

static int cmd_lora(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "info";
    if (!strcmp(sub, "info")) {
        show();
        return 0;
    }
    if (!strcmp(sub, "set") && argc >= 4) return cmd_set(argc, argv);
    if (!strcmp(sub, "send") && argc >= 3) {
        char msg[PAL_LORA_MAX + 1];
        size_t n = 0;
        for (int i = 2; i < argc && n < PAL_LORA_MAX; i++) {
            n += (size_t)snprintf(msg + n, sizeof(msg) - n, "%s%s", i > 2 ? " " : "", argv[i]);
            if (n > PAL_LORA_MAX) n = PAL_LORA_MAX;
        }
        nema_err_t e = pal_lora_send((const uint8_t *)msg, n);
        if (e != NEMA_OK) {
            pal_eprintf("lora: %s\n", why(e));
            return 1;
        }
        pal_lora_cfg_t c;
        pal_lora_cfg_load(&c);
        pal_printf("sent %u bytes (%lu ms on air)\n", (unsigned)n, (unsigned long)pal_lora_airtime_ms(&c, n));
        return 0;
    }
    if (!strcmp(sub, "listen")) {
        int secs = argc > 2 ? atoi(argv[2]) : 0;
        pal_printf("listening%s (Ctrl-C stops)\n", secs ? "" : " until stopped");
        uint64_t end = secs ? pal_sys_uptime_us() + (uint64_t)secs * 1000000u : 0;
        int got = 0;
        while (!nema_interrupted() && (!end || pal_sys_uptime_us() < end)) {
            uint8_t buf[PAL_LORA_MAX + 1];
            pal_lora_meta_t m = {0};
            int r = pal_lora_recv(buf, PAL_LORA_MAX, 1000, &m);
            if (r == NEMA_ERR_INTR) break;
            if (r < 0) {
                pal_eprintf("lora: %s\n", why(r));
                if (r != NEMA_ERR_PROTO) return 1;
                continue;
            }
            if (r == 0) continue;
            got++;
            pal_printf("[%d dBm, %d dB] ", m.rssi_dbm, m.snr_db);
            for (int i = 0; i < r; i++) pal_printf(buf[i] >= 0x20 && buf[i] < 0x7F ? "%c" : "\\x%02x", buf[i]);
            pal_printf("\n");
        }
        pal_printf("%d packets\n", got);
        return 0;
    }
    pal_eprintf("usage: lora [info] | set freq MHZ sf N bw KHZ cr N power DBM | send TEXT | listen [SECONDS]\n");
    return 2;
}

static const nema_app_t LORA_APP = {"lora", "the LoRa radio: info, set, send TEXT, listen", cmd_lora, 8192, 0};

void pal_apps_register_lora(void);
void pal_apps_register_lora(void) { nema_app_register(&LORA_APP); }
