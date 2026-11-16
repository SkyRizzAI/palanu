#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/vk.h"
#include "pal/wifi.h"

static int wifi_status_cmd(void) {
    if (pal_radio_state(PAL_RADIO_WIFI) != PAL_RADIO_ON) {
        pal_printf("not connected (radio %s)\n", pal_radio_state_name(pal_radio_state(PAL_RADIO_WIFI)));
        return 0;
    }
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    if (st.connected) pal_printf("connected to \"%s\"  ip %s  rssi %d dBm  clock %s\n", st.ssid, st.ip, st.rssi, st.time_synced ? "synced" : "not synced");
    else pal_printf("not connected%s\n", st.saved ? " (saved network, reconnecting)" : "");
    return 0;
}

/* Scan and connect need the driver up. The radio is started with "wifi on", never by these. */
static bool wifi_radio_up(void) {
    if (pal_radio_state(PAL_RADIO_WIFI) == PAL_RADIO_ON) return true;
    pal_eprintf("wifi: radio is %s; run 'wifi on' first\n", pal_radio_state_name(pal_radio_state(PAL_RADIO_WIFI)));
    return false;
}

/* wifi connect with no SSID: scan, list the networks strongest first, and take a number. False when none was taken. */
static bool wifi_pick(char *ssid, size_t cap, bool *secure) {
    pal_ap_t *aps = (pal_ap_t *)nema_alloc(NEMA_MEM_ANY, nema_self()->owner, 24 * sizeof(pal_ap_t));
    if (!aps) return false;
    size_t n = 0, order[24];
    pal_puts("scanning...\n");
    nema_err_t r = pal_wifi_scan(aps, 24, &n);
    if (r != NEMA_OK) pal_eprintf("wifi: scan failed: %s\n", pal_strerror(r));
    else if (n == 0) pal_puts("no networks found\n");
    if (r != NEMA_OK || n == 0) {
        nema_free(aps);
        return false;
    }
    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t i = 1; i < n; i++) /* strongest first */
        for (size_t j = i; j > 0 && aps[order[j - 1]].rssi < aps[order[j]].rssi; j--) {
            size_t t = order[j - 1];
            order[j - 1] = order[j];
            order[j] = t;
        }
    for (size_t i = 0; i < n; i++) {
        const pal_ap_t *a = &aps[order[i]];
        pal_printf("%2u  %4d dBm  ch%-2u  %-6s  %s\n", (unsigned)(i + 1), a->rssi, a->channel, a->secure ? "locked" : "open", a->ssid);
    }
    pal_printf("network [1-%u]: ", (unsigned)n);
    char line[16];
    int m = nema_read(0, line, sizeof(line) - 1);
    bool ok = false;
    if (m > 0) {
        line[m] = 0;
        int pick = atoi(line);
        if (pick >= 1 && pick <= (int)n) {
            const pal_ap_t *a = &aps[order[pick - 1]];
            snprintf(ssid, cap, "%s", a->ssid);
            *secure = a->secure;
            ok = true;
        } else {
            pal_eprintf("wifi: no network %d\n", pick);
        }
    }
    nema_free(aps);
    return ok;
}

/* The Wi-Fi password: the on-screen keyboard when there is a display (masked, with a reveal key), else a no-echo line.
 * Returns the length, or -1 when cancelled. */
static int wifi_ask_password(char *out, size_t cap) {
    pal_vk_entry_t e = {"Wi-Fi password", "", out, cap, true, true};
    int r = pal_vk_edit(&e);
    if (r == PAL_VK_CANCEL) return -1;
    if (r >= 0) return r;
    pal_printf("Wi-Fi password: ");
    return pal_read_password(out, cap);
}

static int cmd_wifi(int argc, char **argv) {
    if (!pal_wifi_available()) {
        pal_eprintf("wifi: not available on this board\n");
        return 1;
    }
    if (argc == 1) return wifi_status_cmd();
    if (strcmp(argv[1], "on") == 0) {
        /* persisted even when refused, so the next boot tries again once memory allows */
        pal_settings_set_radio("radio.wifi", "on");
        (void)psys_svc_start("radio-wifi"); /* the service holds the radio; a refusal retries every 5 s */
        if (pal_radio_state(PAL_RADIO_WIFI) != PAL_RADIO_ON) {
            pal_eprintf("wifi: %s\n", pal_radio_last_error(PAL_RADIO_WIFI));
            return 1;
        }
        return wifi_status_cmd();
    }
    if (strcmp(argv[1], "off") == 0) {
        (void)psys_svc_stop("radio-wifi"); /* releases the radio and records the choice (radio.wifi=off) */
        pal_printf("wifi: off%s\n", pal_radio_state(PAL_RADIO_WIFI) == PAL_RADIO_ON ? " (stops after the grace period)" : "");
        return 0;
    }
    if (strcmp(argv[1], "scan") == 0) {
        if (!wifi_radio_up()) return 1;
        pal_ap_t *aps = (pal_ap_t *)nema_alloc(NEMA_MEM_ANY, nema_self()->owner, 24 * sizeof(pal_ap_t));
        if (!aps) return 1;
        size_t n = 0;
        pal_puts("scanning...\n");
        nema_err_t r = pal_wifi_scan(aps, 24, &n);
        if (r != NEMA_OK) pal_eprintf("wifi: scan failed: %s\n", pal_strerror(r));
        for (size_t i = 0; i < n; i++)
            pal_printf("%4d dBm  ch%-2u  %s  %s\n", aps[i].rssi, aps[i].channel, aps[i].secure ? "locked" : "open  ", aps[i].ssid);
        if (r == NEMA_OK && n == 0) pal_puts("no networks found\n");
        nema_free(aps);
        return r == NEMA_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "connect") == 0) {
        /* no SSID: scan and pick; no PASSWORD: ask for it (not asked for a network that is open) */
        if (!wifi_radio_up()) return 1;
        char ssid[33] = "", pass[65] = "";
        bool secure = true;
        if (argc >= 3) snprintf(ssid, sizeof(ssid), "%s", argv[2]);
        else if (!wifi_pick(ssid, sizeof(ssid), &secure)) return 1;
        const char *password = argc > 3 ? argv[3] : NULL;
        if (!password && secure) {
            if (wifi_ask_password(pass, sizeof(pass)) < 0) {
                memset(pass, 0, sizeof(pass));
                pal_eprintf("wifi: cancelled\n");
                return 1;
            }
            password = pass;
        }
        pal_printf("connecting to \"%s\"...\n", ssid);
        nema_err_t r = pal_wifi_connect(ssid, password ? password : "", 20000);
        memset(pass, 0, sizeof(pass));
        if (r != NEMA_OK) {
            pal_eprintf("wifi: %s\n", pal_strerror(r));
            return 1;
        }
        return wifi_status_cmd();
    }
    if (strcmp(argv[1], "disconnect") == 0) return pal_wifi_disconnect() == NEMA_OK ? 0 : 1;
    if (strcmp(argv[1], "forget") == 0) return pal_wifi_forget() == NEMA_OK ? 0 : 1;
    pal_eprintf("usage: wifi [on | off | scan | connect [SSID [PASS]] | disconnect | forget]\n");
    return 1;
}

/* nc HOST PORT: connect and relay between the terminal and the socket. Waits on both with one poll. */
static int cmd_nc(int argc, char **argv) {
    if (argc < 3) {
        pal_eprintf("usage: nc HOST PORT\n");
        return 1;
    }
    nema_obj_t *s;
    nema_err_t r = nema_net_connect(argv[1], (uint16_t)atoi(argv[2]), 8000, &s);
    if (r != NEMA_OK) {
        pal_eprintf("nc: %s:%s: %s\n", argv[1], argv[2], pal_strerror(r));
        return 1;
    }
    int fd = nema_handle_install(s);
    nema_obj_unref(s);
    pal_printf("connected. Ctrl-D ends the input side, Ctrl-C quits.\n");
    char buf[256];
    bool in_open = true;
    for (;;) {
        nema_pollfd_t p[2] = {{fd, NEMA_POLL_IN, 0}, {0, NEMA_POLL_IN, 0}};
        int n = nema_poll(p, in_open ? 2 : 1, -1);
        if (n < 0) break;
        if (p[0].revents) {
            int got = nema_read(fd, buf, sizeof(buf));
            if (got <= 0) break;
            nema_write(1, buf, (size_t)got);
        }
        if (in_open && p[1].revents) {
            int got = nema_read(0, buf, sizeof(buf));
            if (got <= 0) in_open = false;
            else if (nema_write(fd, buf, (size_t)got) < 0) break;
        }
    }
    nema_close(fd);
    return 0;
}

/* curl [-i] http://host[:port]/path : a plain HTTP GET (no TLS yet) */
static int cmd_curl(int argc, char **argv) {
    bool headers = false;
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-i") == 0) {
        headers = true;
        i = 2;
    }
    if (i >= argc || strncmp(argv[i], "http://", 7) != 0) {
        pal_eprintf("usage: curl [-i] http://host[:port]/path   (https is not supported yet)\n");
        return 1;
    }
    char host[64], path[96] = "/";
    uint16_t port = 80;
    const char *u = argv[i] + 7;
    const char *slash = strchr(u, '/');
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    if (hl >= sizeof(host)) return 1;
    memcpy(host, u, hl);
    host[hl] = 0;
    if (slash) nema_strlcpy(path, slash, sizeof(path));
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = (uint16_t)atoi(colon + 1);
    }
    nema_obj_t *s;
    nema_err_t r = nema_net_connect(host, port, 8000, &s);
    if (r != NEMA_OK) {
        pal_eprintf("curl: %s: %s\n", host, pal_strerror(r));
        return 1;
    }
    char req[256];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: palanu\r\nConnection: close\r\n\r\n", path, host);
    nema_obj_write(s, req, (size_t)n, 5000);
    char buf[256];
    bool in_body = headers;
    char tail[4] = {0};
    int got;
    while ((got = nema_obj_read(s, buf, sizeof(buf), 10000)) > 0) {
        for (int k = 0; k < got; k++) {
            if (!in_body) { /* skip headers up to the blank line */
                memmove(tail, tail + 1, 3);
                tail[3] = buf[k];
                if (memcmp(tail, "\r\n\r\n", 4) == 0) in_body = true;
                continue;
            }
            char c = buf[k];
            if (c == '\n') nema_write(1, "\r\n", 2);
            else if (c != '\r') nema_write(1, &c, 1);
        }
    }
    nema_obj_unref(s);
    return got < 0 && got != NEMA_ERR_PIPE ? 1 : 0;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t NET_APPS[] = {
    APP("wifi", "Wi-Fi: scan, connect, status", cmd_wifi, 8192),
    APP("nc", "TCP client: relay stdin and a socket", cmd_nc, 8192),
    APP("curl", "HTTP GET (no TLS yet)", cmd_curl, 8192),
};

void pal_apps_register_net(void);
void pal_apps_register_net(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(NET_APPS); i++) nema_app_register(&NET_APPS[i]);
}
