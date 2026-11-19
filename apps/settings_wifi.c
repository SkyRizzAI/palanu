/* Settings pages: Wi-Fi, the connected network, the saved networks and one saved network. The Wi-Fi page is the reference's: the
 * switch, the connected network, My networks, the scan results under Other networks, Rescan and Add other network. The radio
 * starts and stops, the scan and the connect run as jobs (settings_job.c); the password is asked on the virtual keyboard, masked.
 * A saved network has its own page: Auto-Join, Automatic or Manual IPv4 (address, subnet mask, router, DNS) and Forget. */
#include <stdio.h>
#include <string.h>

#include "nema/types.h"
#include "pal/radio.h"
#include "pal/wifi.h"
#include "settings_int.h"

#define SCAN_MAX 24
#define NET_STORE 28  /* scan rows of the older list: up to 24 networks, plus the header rows */
#define WIFI_STORE_N 32 /* Wi-Fi page: switch, add, two section rows, the connected row and up to 24 networks */

static pal_ap_t s_aps[SCAN_MAX] NEMA_EXT_BSS;
static size_t s_naps;
static pal_menu_row_t WIFI_STORE[WIFI_STORE_N] NEMA_EXT_BSS;
static pal_menu_row_t SCAN_STORE[NET_STORE] NEMA_EXT_BSS;
static pal_menu_row_t DETAIL_STORE[10] NEMA_EXT_BSS;
static pal_menu_row_t SAVED_STORE[PAL_WIFI_NETS + 2] NEMA_EXT_BSS;
static pal_menu_row_t NET_STORE_ROWS[12] NEMA_EXT_BSS;
static pal_wifi_net_t s_saved[PAL_WIFI_NETS] NEMA_EXT_BSS;
static char s_net[33]; /* the saved network the one-network page shows */
static char s_ssid[33];            /* the network being added or joined, across the two questions */
static char s_title[48];           /* the password keyboard's title: "Password: <ssid>" */

static bool radio_on(void) { return pal_radio_state(PAL_RADIO_WIFI) == PAL_RADIO_ON; }

/* ---- the switch ---- */

static int wifi_get(void *ctx) {
    (void)ctx;
    pal_radio_state_t s = pal_radio_state(PAL_RADIO_WIFI);
    return s == PAL_RADIO_ON || s == PAL_RADIO_STARTING;
}

static void wifi_set(void *ctx, int v) {
    (void)ctx;
    if (v) {
        settings_run_job(JOB_WIFI_ON, NULL, NULL, "Starting Wi-Fi...");
    } else {
        settings_run_job(JOB_WIFI_OFF, NULL, NULL, "Turning Wi-Fi off...");
    }
}

/* ---- connect and password ---- */

/* The reference prompt for a password: the network's name is in it. The keyboard copies the title, so one buffer serves. */
static const char *password_title(const char *ssid) {
    snprintf(s_title, sizeof(s_title), "Password: %s", ssid ? ssid : "");
    return s_title;
}

static void connect_done(void *ctx, bool ok, const char *text) {
    (void)ctx;
    if (!ok) return;
    settings_run_job(JOB_CONNECT, s_ssid, text ? text : "", "Connecting...");
}

void settings_wifi_connect_ask(const char *ssid, bool secure) {
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid ? ssid : "");
    if (!secure) {
        settings_run_job(JOB_CONNECT, s_ssid, "", "Connecting...");
        return;
    }
    pal_menu_ask_text(settings_menu(), password_title(s_ssid), "", 64, true, connect_done, NULL);
}

static void add_ssid_done(void *ctx, bool ok, const char *text) {
    (void)ctx;
    if (!ok || !text || !text[0]) return;
    snprintf(s_ssid, sizeof(s_ssid), "%s", text);
    pal_menu_ask_text(settings_menu(), password_title(s_ssid), "", 64, true, connect_done, NULL);
}

/* Add Other Network: the name first. A submenu row (for its chevron) that opens nothing. */
static pal_menu_page_t *add_network_open(void *ctx) {
    (void)ctx;
    if (!radio_on()) {
        settings_toast("Turn Wi-Fi on first");
        return NULL;
    }
    pal_menu_ask_text(settings_menu(), "Network Name", "", 32, false, add_ssid_done, NULL);
    return NULL;
}

/* ---- status readings ---- */

static bool connected(void) {
    if (!radio_on()) return false;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    return st.connected;
}

static pal_menu_page_t *open_detail(void *ctx) {
    (void)ctx;
    return connected() ? &PG_WIFI_DETAIL : NULL;
}

static void rescan_run(void *ctx) {
    (void)ctx;
    if (!radio_on()) {
        settings_toast("Turn Wi-Fi on first");
        return;
    }
    settings_run_job(JOB_SCAN, NULL, NULL, "Scanning...");
}

/* ---- Wi-Fi ---- */

static void ap_run(void *ctx) {
    const pal_ap_t *a = (const pal_ap_t *)ctx;
    if (!radio_on()) {
        settings_toast("Turn Wi-Fi on first");
        return;
    }
    settings_wifi_connect_ask(a->ssid, a->secure);
}

/* Scan results, strongest first (copied from the job's store on every build). */
static void load_scan(void) {
    s_naps = settings_scan_get(s_aps, SCAN_MAX);
    for (size_t i = 1; i < s_naps; i++)
        for (size_t j = i; j > 0 && s_aps[j - 1].rssi < s_aps[j].rssi; j--) {
            pal_ap_t t = s_aps[j - 1];
            s_aps[j - 1] = s_aps[j];
            s_aps[j] = t;
        }
}

/* The Network rows (pre-port layout): the connected network's name, the saved network, and the scan results as a page. */
static void connected_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    if (!radio_on()) {
        snprintf(out, cap, "radio off");
        return;
    }
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    snprintf(out, cap, "%s", st.connected ? st.ssid : "none");
}

static void saved_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_wifi_net_t n[PAL_WIFI_NETS];
    size_t k = pal_wifi_saved(n, PAL_WIFI_NETS);
    if (k == 0) snprintf(out, cap, "none");
    else snprintf(out, cap, "%u saved", (unsigned)k);
}

static void scan_count_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    if (!settings_scan_done()) snprintf(out, cap, "not scanned");
    else snprintf(out, cap, "%u found", (unsigned)s_naps);
}

/* Add Other Network: asks the name first; the password follows (settings_wifi_connect_ask). */
static void add_network_run(void *ctx) {
    (void)ctx;
    (void)add_network_open(NULL);
}

static void build_wifi(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r;
    if (!pal_wifi_available()) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Wi-Fi", .text = settings_text_static,
                             .ctx = (void *)"Not available on this board"};
        pal_menu_add(p, &r);
        return;
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Wi-Fi", .icon = "wifi", .get = wifi_get, .set = wifi_set,
                         .key = "radio.wifi"};
    pal_menu_add(p, &r);
    if (!wifi_get(NULL)) return; /* the reference: the rows are left out while the radio is off */

    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Network"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Connected", .icon = "wifi", .text = connected_text,
                         .open = open_detail, .disabled = !connected(), .hint = "Enter shows the address and the forget action"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "My networks", .icon = "lock", .text = saved_text,
                         .open = settings_open_ctx, .ctx = &PG_WIFI_SAVED, .hint = "Saved networks: Auto-Join, IP address, forget"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Other networks", .icon = "wifi", .text = scan_count_text,
                         .open = settings_open_ctx, .ctx = &PG_WIFI_SCAN, .hint = "Scan results; Enter on one joins it"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Rescan", .icon = "wifi", .run = rescan_run,
                         .hint = "Looks for networks in range (a few seconds)"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Add other network", .icon = "lock", .run = add_network_run,
                         .hint = "Type the name and the password"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_WIFI = {.title = "Wi-Fi", .store = WIFI_STORE, .cap = WIFI_STORE_N, .build = build_wifi};

/* ---- scan results (the list the Wi-Fi page used before; kept for the wifi-scan page) ---- */

static const char *bars_of(int rssi) {
    if (rssi > -55) return "||||";
    if (rssi > -65) return "|||.";
    if (rssi > -75) return "||..";
    return "|...";
}

static void ap_text(void *ctx, char *out, size_t cap) {
    const pal_ap_t *a = (const pal_ap_t *)ctx;
    snprintf(out, cap, "%s %d", bars_of(a->rssi), (int)a->rssi);
}

static void build_scan(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Rescan", .icon = "wifi", .run = rescan_run};
    pal_menu_add(p, &r);
    load_scan();
    for (size_t i = 0; i < s_naps; i++) {
        const pal_ap_t *a = &s_aps[i];
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = a->ssid[0] ? a->ssid : "(hidden)",
                             .icon = a->secure ? "lock" : NULL, .ctx = (void *)a, .text = ap_text, .show_value = true,
                             .run = ap_run,
                             .hint = a->secure ? "Locked: asks for the password" : "Open: joins at once"};
        pal_menu_add(p, &r);
    }
    if (!settings_scan_done()) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Not scanned yet", .text = settings_text_static,
                             .ctx = (void *)"Use Rescan"};
        pal_menu_add(p, &r);
    } else if (s_naps == 0) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "No networks found", .text = settings_text_static,
                             .ctx = (void *)"Move closer or rescan"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_WIFI_SCAN = {.title = "Other networks", .store = SCAN_STORE, .cap = NET_STORE, .build = build_scan,
                                .live = false};

/* ---- connected network: address, signal, actions ---- */

static void disconnect_run(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_DISCONNECT, NULL, NULL, "Disconnecting...");
}

/* The row asks first (its confirm text); this runs when the user says Forget. */
static void forget_run(void *ctx) { /* the connected network only: the others stay saved */
    (void)ctx;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    settings_run_job(JOB_FORGET, st.ssid, NULL, "Forgetting network...");
}

static void status_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    snprintf(out, cap, "%s", st.connected ? "Connected" : "Not connected");
}

static void ssid_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    snprintf(out, cap, "%s", st.connected && st.ssid[0] ? st.ssid : "-");
}

static void ip_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    snprintf(out, cap, "%s", st.ip[0] ? st.ip : "none");
}

static void signal_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    if (!st.connected) snprintf(out, cap, "-");
    else snprintf(out, cap, "%d dBm, %d of 4 bars", (int)st.rssi, pal_wifi_bars());
}

static void clock_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    snprintf(out, cap, "%s", st.time_synced ? "synced" : "not synced");
}

static void build_detail(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Status", .text = status_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Network", .text = ssid_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "IP address", .text = ip_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Signal", .text = signal_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Clock (SNTP)", .text = clock_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Disconnect", .icon = "wifi", .run = disconnect_run,
                         .hint = "Stays saved; it reconnects when the radio is on"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Forget network", .icon = "lock", .danger = true,
                         .run = forget_run, .confirm = "This network's saved password is erased.", .ok_label = "Forget"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_WIFI_DETAIL = {.title = "Wi-Fi network", .store = DETAIL_STORE, .cap = 10, .build = build_detail,
                                  .live = true};

/* ---- My networks: the saved ones, most recent first ---- */

static pal_menu_page_t *open_net(void *ctx) {
    const pal_wifi_net_t *n = (const pal_wifi_net_t *)ctx;
    snprintf(s_net, sizeof(s_net), "%s", n->ssid);
    return &PG_WIFI_NET;
}

static void saved_flags_text(void *ctx, char *out, size_t cap) {
    const pal_wifi_net_t *n = (const pal_wifi_net_t *)ctx;
    snprintf(out, cap, "%s%s", n->auto_join ? "auto" : "manual join", n->manual_ip ? ", fixed IP" : "");
}

static void build_saved(pal_menu_page_t *p) {
    pal_menu_clear(p);
    size_t k = pal_wifi_saved(s_saved, PAL_WIFI_NETS);
    for (size_t i = 0; i < k; i++) {
        pal_menu_row_t r = {.kind = PAL_MENU_SUBMENU, .label = s_saved[i].ssid, .icon = s_saved[i].secure ? "lock" : "wifi",
                            .ctx = &s_saved[i], .text = saved_flags_text, .open = open_net,
                            .hint = "Auto-Join, the IP address, forget"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_WIFI_SAVED = {.title = "My networks", .store = SAVED_STORE, .cap = PAL_WIFI_NETS + 2, .build = build_saved,
                                 .empty = "No saved networks: join one and it is kept here"};

/* ---- one saved network ---- */

static bool net_get(pal_wifi_net_t *n) { return pal_wifi_saved_get(s_net, n); }

static int auto_get(void *ctx) {
    (void)ctx;
    pal_wifi_net_t n;
    return net_get(&n) && n.auto_join;
}
static void auto_set(void *ctx, int v) {
    (void)ctx;
    (void)pal_wifi_set_auto(s_net, v != 0);
}

static const char *const IP_MODES[] = {"Automatic", "Manual", NULL};
static int mode_get(void *ctx) {
    (void)ctx;
    pal_wifi_net_t n;
    return net_get(&n) && n.manual_ip;
}
static void mode_set(void *ctx, int v) {
    (void)ctx;
    pal_wifi_net_t n;
    if (!net_get(&n)) return;
    if (pal_wifi_set_ip(s_net, v != 0, &n.ip) == NEMA_ERR_INVAL) settings_toast("Enter the IP address first");
    else settings_toast(v ? "Manual address from the next connect" : "DHCP from the next connect");
}

/* The four addresses: ctx is the field's offset in pal_ipv4_conf_t (0 ip, 4 mask, 8 router, 12 DNS). */
static void field_text(void *ctx, char *out, size_t cap) {
    pal_wifi_net_t n;
    const uint8_t *a = net_get(&n) ? (const uint8_t *)&n.ip + (uintptr_t)ctx : NULL;
    if (!a || !(a[0] | a[1] | a[2] | a[3])) snprintf(out, cap, "-");
    else pal_ipv4_text(a, out, cap);
}
static bool field_apply(void *ctx, const char *text) {
    pal_wifi_net_t n;
    uint8_t v[4];
    if (!net_get(&n) || !pal_ipv4_parse(text, v)) {
        settings_toast("Four numbers 0 to 255, like 192.168.1.20");
        return false;
    }
    memcpy((uint8_t *)&n.ip + (uintptr_t)ctx, v, 4);
    return pal_wifi_set_ip(s_net, n.manual_ip, &n.ip) == NEMA_OK;
}

static void net_name_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", s_net);
}

static void forget_net_run(void *ctx) {
    (void)ctx;
    settings_run_job(JOB_FORGET, s_net, NULL, "Forgetting network...");
    (void)pal_menu_pop(settings_menu()); /* the page is about a network that is gone */
}

static void build_net(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "Network", .text = net_name_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Auto-Join", .icon = "wifi", .get = auto_get, .set = auto_set,
                         .hint = "Joins by itself when in range"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "IPv4"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_CHOICE, .label = "Configure IP", .choices = IP_MODES, .get = mode_get, .set = mode_set,
                         .hint = "Automatic is DHCP; Manual uses the addresses below"};
    pal_menu_add(p, &r);
    static const char *const LABELS[] = {"IP address", "Subnet mask", "Router", "DNS"};
    for (int i = 0; i < 4; i++) {
        r = (pal_menu_row_t){.kind = PAL_MENU_TEXT, .label = LABELS[i], .ctx = (void *)(uintptr_t)(i * 4), .text = field_text,
                             .apply_text = field_apply, .max_len = 15, .hint = "Used when Configure IP is Manual"};
        pal_menu_add(p, &r);
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Forget this network", .icon = "lock", .danger = true,
                         .run = forget_net_run, .confirm = "Its saved password is erased.", .ok_label = "Forget"};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_WIFI_NET = {.title = "Network", .store = NET_STORE_ROWS, .cap = 12, .build = build_net};
