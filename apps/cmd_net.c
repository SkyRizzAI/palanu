#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "libapp.h"
#include "pal/http.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/sysinfo.h"
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

/* wifi saved: the saved networks, the most recent first. */
static int wifi_saved_cmd(void) {
    pal_wifi_net_t n[PAL_WIFI_NETS];
    size_t k = pal_wifi_saved(n, PAL_WIFI_NETS);
    if (k == 0) pal_printf("no saved networks\n");
    for (size_t i = 0; i < k; i++) {
        char ip[16] = "dhcp";
        if (n[i].manual_ip) pal_ipv4_text(n[i].ip.ip, ip, sizeof(ip));
        pal_printf("%u  %-6s  %-4s  %-15s  %s\n", (unsigned)(i + 1), n[i].secure ? "locked" : "open", n[i].auto_join ? "auto" : "-", ip,
                   n[i].ssid);
    }
    return 0;
}

/* wifi ip SSID dhcp | wifi ip SSID ADDRESS MASK ROUTER [DNS]: from the next time it joins. */
static int wifi_ip_cmd(int argc, char **argv) {
    if (!strcmp(argv[3], "dhcp")) {
        nema_err_t r = pal_wifi_set_ip(argv[2], false, NULL);
        if (r == NEMA_ERR_NOENT) pal_eprintf("wifi: \"%s\" is not saved\n", argv[2]);
        return r == NEMA_OK ? 0 : 1;
    }
    pal_ipv4_conf_t c;
    memset(&c, 0, sizeof(c));
    if (argc < 6 || !pal_ipv4_parse(argv[3], c.ip) || !pal_ipv4_parse(argv[4], c.mask) || !pal_ipv4_parse(argv[5], c.gw) ||
        (argc > 6 && !pal_ipv4_parse(argv[6], c.dns))) {
        pal_eprintf("usage: wifi ip SSID ADDRESS MASK ROUTER [DNS]   (each four numbers, like 192.168.1.20)\n");
        return 1;
    }
    if (!c.dns[0]) memcpy(c.dns, c.gw, 4); /* no DNS given: the router answers names, as most do */
    nema_err_t r = pal_wifi_set_ip(argv[2], true, &c);
    if (r == NEMA_ERR_NOENT) pal_eprintf("wifi: \"%s\" is not saved (join it first)\n", argv[2]);
    else if (r == NEMA_OK) pal_printf("wifi: \"%s\" uses %s from the next connect\n", argv[2], argv[3]);
    return r == NEMA_OK ? 0 : 1;
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
    if (strcmp(argv[1], "forget") == 0) { /* one network by name, or every saved one */
        nema_err_t r = argc > 2 ? pal_wifi_forget_net(argv[2]) : pal_wifi_forget();
        if (r == NEMA_ERR_NOENT) pal_eprintf("wifi: \"%s\" is not saved (`wifi saved`)\n", argv[2]);
        return r == NEMA_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "saved") == 0) return wifi_saved_cmd();
    if (strcmp(argv[1], "auto") == 0 && argc == 4 && (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
        nema_err_t r = pal_wifi_set_auto(argv[2], !strcmp(argv[3], "on"));
        if (r == NEMA_ERR_NOENT) pal_eprintf("wifi: \"%s\" is not saved\n", argv[2]);
        return r == NEMA_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "ip") == 0 && argc >= 4) return wifi_ip_cmd(argc, argv);
    pal_eprintf("usage: wifi [on | off | scan | connect [SSID [PASS]] | disconnect | saved | forget [SSID] |\n"
                "             auto SSID on|off | ip SSID dhcp | ip SSID ADDRESS MASK ROUTER [DNS]]\n");
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

/* curl: an HTTP and HTTPS client on the system's (pal/http.h).
 *   curl [-i] [-I] [-s] [-k] [-L] [-X METHOD] [-H 'Name: value']... [-d DATA] [-o FILE] URL
 * -i prints the status line and headers before the body, -I sends HEAD and prints only them, -s keeps errors quiet, -k accepts any
 * certificate, -L follows redirects (up to 8), -d sends DATA (POST unless -X says otherwise), -o writes the body to FILE. */
#define CURL_HEADERS 8
typedef struct {
    bool show_headers, quiet;
    nema_obj_t *file; /* -o */
} curl_out_t;

static void curl_header(void *ctx, const char *line) {
    curl_out_t *o = (curl_out_t *)ctx;
    if (o->show_headers) pal_printf("%s\n", line);
}

static int curl_data(void *ctx, const void *buf, size_t n) {
    curl_out_t *o = (curl_out_t *)ctx;
    if (o->file) return nema_obj_write(o->file, buf, n, -1) == (int)n ? 0 : -1;
    return nema_write(1, buf, n) < 0 ? -1 : 0;
}

static bool starts_ci(const char *s, const char *prefix) { /* ASCII, case-insensitive */
    for (; *prefix; s++, prefix++)
        if ((*s | 32) != (*prefix | 32)) return false;
    return true;
}

static int cmd_curl(int argc, char **argv) {
    const char *headers[CURL_HEADERS + 2];
    int nh = 0;
    pal_http_req_t q = {.timeout_ms = 15000};
    curl_out_t out = {0};
    const char *url = NULL, *file = NULL;
    bool head_only = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        /* letters that take no value may be joined, as curl allows: -sIL */
        bool flags = a[0] == '-' && a[1] && strspn(a + 1, "iIskL") == strlen(a + 1);
        if (flags) {
            for (const char *f = a + 1; *f; f++) {
                if (*f == 'i') out.show_headers = true;
                else if (*f == 'I') head_only = out.show_headers = true;
                else if (*f == 's') out.quiet = true;
                else if (*f == 'k') q.insecure = true;
                else q.redirects = 8;
            }
        } else if (!strcmp(a, "-X") && more) q.method = argv[++i];
        else if (!strcmp(a, "-H") && more && nh < CURL_HEADERS) headers[nh++] = argv[++i];
        else if (!strcmp(a, "-d") && more) {
            q.body = argv[++i];
            q.body_len = strlen(argv[i]);
        } else if (!strcmp(a, "-o") && more) file = argv[++i];
        else if (a[0] != '-' && !url) url = a;
        else {
            url = NULL;
            break;
        }
    }
    if (!url) {
        pal_eprintf("usage: curl [-i] [-I] [-s] [-k] [-L] [-X METHOD] [-H 'Name: value'] [-d DATA] [-o FILE] URL\n");
        return 2;
    }
    bool typed = false; /* -d sends a form unless a -H gives the body's type */
    for (int i = 0; i < nh; i++) typed |= starts_ci(headers[i], "content-type:");
    if (q.body && !typed) headers[nh++] = "Content-Type: application/x-www-form-urlencoded";
    headers[nh] = NULL;
    q.headers = nh ? headers : NULL;
    if (head_only) q.method = "HEAD";
    q.url = url;
    q.on_header = curl_header;
    q.on_data = curl_data;
    q.ctx = &out;
    if (file && nema_fs_open(file, NEMA_O_WRITE | NEMA_O_CREATE | NEMA_O_TRUNC, &out.file) != NEMA_OK) {
        pal_eprintf("curl: cannot write %s\n", file);
        return 1;
    }
    pal_http_resp_t r;
    nema_err_t e = pal_http_request(&q, &r);
    if (out.file) nema_obj_unref(out.file);
    if (out.show_headers && e == NEMA_OK) pal_printf("\n");
    if (e != NEMA_OK) {
        if (!out.quiet) {
            if (e == NEMA_ERR_AUTH) pal_eprintf("curl: %s: the server's certificate is not trusted (-k skips the check)\n", url);
            else if (e == NEMA_ERR_UNSUPPORTED) pal_eprintf("curl: https is not available in this build\n");
            else if (e == NEMA_ERR_INVAL) pal_eprintf("curl: %s: not an http:// or https:// URL\n", url);
            else if (e == NEMA_ERR_RANGE) pal_eprintf("curl: too many redirects\n");
            else pal_eprintf("curl: %s: %s\n", url, pal_strerror(e));
        }
        return 1;
    }
    if (file && !out.quiet) pal_printf("%s: %llu bytes, HTTP %d\n", file, (unsigned long long)r.received, r.status);
    return r.status >= 400 ? 22 : 0; /* curl --fail's exit code: scripts can test it */
}

/* ping [-c COUNT] HOST: ICMP echo, one a second, then the summary. Ctrl-C stops early (the summary still prints). */
static int cmd_ping(int argc, char **argv) {
    int count = 4;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) count = atoi(argv[++i]);
        else if (argv[i][0] != '-') host = argv[i];
    }
    if (!host || count < 1) {
        pal_eprintf("usage: ping [-c COUNT] HOST\n");
        return 2;
    }
    int sent = 0, got = 0;
    uint32_t min = 0xFFFFFFFFu, max = 0;
    uint64_t total = 0;
    for (int i = 0; i < count; i++) {
        uint32_t rtt = 0;
        uint8_t ip[4] = {0};
        nema_err_t r = nema_net_ping(host, (uint16_t)(i + 1), 2000, &rtt, ip);
        if (r == NEMA_ERR_INTR || r == NEMA_ERR_CANCELED) break;
        if (r == NEMA_ERR_NOENT || r == NEMA_ERR_UNSUPPORTED) {
            pal_eprintf("ping: %s: %s\n", host, r == NEMA_ERR_NOENT ? "unknown host" : "ICMP is not available here");
            return 2;
        }
        sent++;
        if (r == NEMA_OK) {
            got++;
            total += rtt, min = rtt < min ? rtt : min, max = rtt > max ? rtt : max;
            pal_printf("reply from %u.%u.%u.%u: seq=%d time=%u.%u ms\n", ip[0], ip[1], ip[2], ip[3], i + 1, (unsigned)(rtt / 1000),
                       (unsigned)(rtt % 1000 / 100));
        } else {
            pal_printf("seq=%d: no reply (%s)\n", i + 1, pal_strerror(r));
        }
        if (i + 1 < count && nema_sleep_ms(rtt >= 1000000u ? 0 : (int32_t)(1000 - rtt / 1000)) != NEMA_OK) break;
    }
    pal_printf("--- %s: %d sent, %d received, %d%% lost", host, sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got) pal_printf(", time min/avg/max %u.%u/%u.%u/%u.%u ms", (unsigned)(min / 1000), (unsigned)(min % 1000 / 100),
                        (unsigned)(total / (uint64_t)got / 1000), (unsigned)(total / (uint64_t)got % 1000 / 100), (unsigned)(max / 1000),
                        (unsigned)(max % 1000 / 100));
    pal_printf("\n");
    return got ? 0 : 1;
}

/* tcping [-c COUNT] HOST PORT: how long a TCP connect takes (works where ICMP is filtered). */
static int cmd_tcping(int argc, char **argv) {
    int count = 4;
    const char *host = NULL, *port = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!host) host = argv[i];
        else port = argv[i];
    }
    int p = port ? atoi(port) : 0;
    if (!host || p < 1 || p > 65535 || count < 1) {
        pal_eprintf("usage: tcping [-c COUNT] HOST PORT\n");
        return 2;
    }
    int ok = 0;
    for (int i = 0; i < count; i++) {
        uint64_t t0 = pal_sys_uptime_us();
        nema_obj_t *s = NULL;
        nema_err_t r = nema_net_connect(host, (uint16_t)p, 3000, &s);
        uint32_t us = (uint32_t)(pal_sys_uptime_us() - t0);
        if (r == NEMA_ERR_INTR || r == NEMA_ERR_CANCELED) break;
        if (r == NEMA_OK) {
            ok++;
            nema_obj_unref(s);
            pal_printf("%s:%d open, %u.%u ms\n", host, p, (unsigned)(us / 1000), (unsigned)(us % 1000 / 100));
        } else {
            pal_printf("%s:%d %s\n", host, p, pal_strerror(r));
        }
        if (i + 1 < count && nema_sleep_ms(500) != NEMA_OK) break;
    }
    return ok ? 0 : 1;
}

#define APP(n, h, f, s) {n, h, f, s, 0}
static const nema_app_t NET_APPS[] = {
    APP("wifi", "Wi-Fi: scan, connect, status", cmd_wifi, 8192),
    APP("nc", "TCP client: relay stdin and a socket", cmd_nc, 8192),
    APP("curl", "HTTP and HTTPS client", cmd_curl, 8192),
    APP("ping", "ICMP echo: is a host there, and how far", cmd_ping, 6144),
    APP("tcping", "time a TCP connect to a host and port", cmd_tcping, 6144),
};

void pal_apps_register_net(void);
void pal_apps_register_net(void) {
    for (size_t i = 0; i < NEMA_ARRAY_SIZE(NET_APPS); i++) nema_app_register(&NET_APPS[i]);
}
