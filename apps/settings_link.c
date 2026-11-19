/* Settings pages: Bluetooth and Remote (the reference rows: the BLE switch and the paired devices; Remote Enabled, Password and the
 * sessions), the Palanu Link page (cable trust, pairing, the paired devices, the TCP server), and the pairing overlay that shows the 6-digit code large with its countdown. Pairing and
 * revoking go through pal/link.h (link_admin.c); the cable trust is the settings key link.uart.trust. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nema/types.h"
#include "pal/board.h"
#include "pal/desktop.h"
#include "pal/link.h"
#include "pal/overlay.h"
#include "pal/radio.h"
#include "pal/svc.h"
#include "pal/theme.h"
#include "pal/time.h"
#include "pal/toast.h"
#include "pal/ui.h"
#include "pal/wifi.h"
#include "settings_int.h"

#define CLIENTS_MAX 8

static pal_menu_row_t CLIENTS_STORE[CLIENTS_MAX + 2] NEMA_EXT_BSS;
static char s_ids[CLIENTS_MAX][17] NEMA_EXT_BSS;
static char s_names[CLIENTS_MAX][26] NEMA_EXT_BSS;
static char s_hints[CLIENTS_MAX][64] NEMA_EXT_BSS; /* "last used 3d ago, paired 2026-11-19" for each row */
static char s_code[PAL_LINK_CODE_LEN + 1];

/* ---- paired devices (from pal/link.h): one line per client, parsed into rows. Shared by Bluetooth, Remote and the
 * Palanu Link page; the rows of a page are its own, the strings are the parsed copies below. ---- */

static void revoke_run(void *ctx) {
    const char *id = (const char *)ctx;
    nema_err_t r = pal_link_revoke(id);
    settings_toast(r == NEMA_OK ? "Device revoked" : "Could not revoke");
    pal_menu_refresh(settings_menu());
}

static void add_clients(pal_menu_page_t *p) {
    char text[512];
    size_t n = pal_link_clients_text(text, sizeof(text));
    int count = 0;
    char *save = NULL;
    /* a line: "<16 hex>  <name>  created N  last used N" */
    for (char *line = n ? strtok_r(text, "\n", &save) : NULL; line && count < CLIENTS_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        if (strlen(line) < 18) continue;
        memcpy(s_ids[count], line, 16);
        s_ids[count][16] = 0;
        const char *name = line + 18;
        const char *end = strstr(name, "  created");
        size_t len = end ? (size_t)(end - name) : strlen(name);
        while (len > 0 && name[len - 1] == ' ') len--;
        if (len >= sizeof(s_names[count])) len = sizeof(s_names[count]) - 1;
        memcpy(s_names[count], name, len);
        s_names[count][len] = 0;
        /* two browsers both called "Web Forge" are told apart by when they were used */
        const char *lu = strstr(line, "last used ");
        const char *cr = strstr(line, "created ");
        uint32_t last = lu ? (uint32_t)strtoul(lu + 10, NULL, 10) : 0, made = cr ? (uint32_t)strtoul(cr + 8, NULL, 10) : 0;
        char age[16] = "never";
        if (last) pal_ui_fmt_age(pal_time_epoch(), last, pal_time_tz_minutes(), age, sizeof(age));
        pal_tm_t tm;
        pal_time_from_epoch(made + (uint32_t)(int32_t)(pal_time_tz_minutes() * 60), &tm);
        size_t al = strlen(age);
        bool clock = strchr(age, ':') != NULL, ago = al && (age[al - 1] == 'm' || age[al - 1] == 'd'); /* "14:05", or "5m" / "3d" */
        snprintf(s_hints[count], sizeof(s_hints[count]), "Last used %s%s%s, paired %04d-%02d-%02d", clock ? "at " : "", age,
                 ago ? " ago" : "", tm.year, tm.month, tm.day);
        pal_menu_row_t r = {.kind = PAL_MENU_ACTION, .label = s_names[count], .icon = "link", .ctx = s_ids[count],
                            .run = revoke_run, .confirm = "This device must pair again to connect.",
                            .ok_label = "Revoke", .danger = true, .hint = s_hints[count]};
        pal_menu_add(p, &r);
        count++;
    }
    if (count == 0) {
        pal_menu_row_t r = {.kind = PAL_MENU_INFO, .label = "No paired devices"};
        pal_menu_add(p, &r);
    }
}

static void build_clients(pal_menu_page_t *p) {
    pal_menu_clear(p);
    add_clients(p);
}

pal_menu_page_t PG_LINK_CLIENTS = {.title = "Paired devices", .store = CLIENTS_STORE, .cap = CLIENTS_MAX + 2,
                                   .build = build_clients};

/* The number of paired devices: rows show it on every redraw (each key, and twice a second on a live page). It used to decrypt
 * every record to count lines, which made the Remote pages lag by seconds on the board; the record store keeps the count now. */
static int paired_count(void) { return pal_link_clients_count(); }

static void count_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%d", paired_count());
}

/* ---- Bluetooth: the switch, the advertised name and the paired devices. A board
 * without the radio says so, as the reference does. ---- */

static int ble_get(void *ctx) {
    (void)ctx;
    pal_radio_state_t s = pal_radio_state(PAL_RADIO_BLE);
    return s == PAL_RADIO_ON || s == PAL_RADIO_STARTING;
}

static void ble_set(void *ctx, int v) {
    (void)ctx;
    settings_run_job(v ? JOB_BLE_ON : JOB_BLE_OFF, NULL, NULL, v ? "Starting Bluetooth..." : "Turning Bluetooth off...");
}

static void link_ble_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    if (pal_radio_state(PAL_RADIO_BLE) != PAL_RADIO_ON) snprintf(out, cap, "off");
    else snprintf(out, cap, "%s", pal_link_connected() ? "client connected" : "advertising");
}

static char s_adv[48];

static pal_menu_row_t BT_STORE[CLIENTS_MAX + 8] NEMA_EXT_BSS;

static void build_bluetooth(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r;
    if (!pal_has(PAL_CAP_BLE)) {
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "No Bluetooth driver"};
        pal_menu_add(p, &r);
        return;
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Bluetooth", .icon = "bluetooth", .get = ble_get, .set = ble_set,
                         .key = "radio.ble"};
    pal_menu_add(p, &r);
    if (ble_get(NULL)) {
        char name[32];
        settings_key_text("device.name", name, sizeof(name));
        snprintf(s_adv, sizeof(s_adv), "Advertising as %s", name);
        r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = s_adv};
        pal_menu_add(p, &r);
    }
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Paired Devices"};
    pal_menu_add(p, &r);
    add_clients(p);
}

pal_menu_page_t PG_BLUETOOTH = {.title = "Bluetooth", .store = BT_STORE, .cap = CLIENTS_MAX + 8, .build = build_bluetooth,
                                .live = true};

/* ---- Remote: Connection, Pairing and Sessions. Remote Enabled is the Palanu Link servers
 * (TCP for the command line, WebSocket for Web Forge). PalanuOS has no remote password: a computer pairs once with a code
 * and then signs in with the token it was given (docs/PLP.md), so the reference Password rows are a Pair row here. ---- */

static bool link_running(const char *id) {
    psys_svc_info_t in;
    return psys_svc_find(id, &in) && in.state == PSYS_SVC_RUNNING;
}

static bool link_found(void) {
    psys_svc_info_t in;
    return psys_svc_find("link-tcp", &in) || psys_svc_find("link-ws", &in);
}

static int remote_get(void *ctx) {
    (void)ctx;
    return link_running("link-tcp") || link_running("link-ws");
}

static void remote_set(void *ctx, int v) {
    (void)ctx;
    settings_run_job(v ? JOB_REMOTE_ON : JOB_REMOTE_OFF, NULL, NULL, v ? "Starting..." : "Stopping...");
}

/* Where Web Forge connects: the device's address and the WebSocket port. */
static void address_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    if (!st.connected || !st.ip[0]) snprintf(out, cap, "no Wi-Fi address");
    else if (!link_running("link-ws")) snprintf(out, cap, "%s (remote off)", st.ip);
    else snprintf(out, cap, "%s:7001", st.ip);
}

static void revoke_all_run(void *ctx) {
    (void)ctx;
    nema_err_t r = pal_link_revoke("all");
    settings_toast(r == NEMA_OK ? "All devices revoked" : "Could not revoke");
    pal_menu_refresh(settings_menu());
}

static void pair_run(void *ctx);

static pal_menu_row_t REMOTE_STORE[10] NEMA_EXT_BSS;

static void build_remote(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Connection"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Remote Enabled", .get = remote_get, .set = remote_set,
                         .disabled = !link_found(), .hint = "This build has no Palanu Link server"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .inert = true, .label = "Address", .text = address_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Pairing"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Pair a Device", .icon = "link", .run = pair_run,
                         .hint = "Shows a 6-digit code for 120 s"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Sessions"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Authorized Devices", .text = count_text,
                         .open = settings_open_ctx, .ctx = &PG_LINK_CLIENTS};
    pal_menu_add(p, &r);
    if (paired_count() > 0) {
        r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Log Out All Devices", .icon = "lock", .danger = true,
                             .run = revoke_all_run, .confirm = "Every paired device must pair again.", .ok_label = "Log Out"};
        pal_menu_add(p, &r);
    }
}

pal_menu_page_t PG_REMOTE = {.title = "Remote", .store = REMOTE_STORE, .cap = 10, .build = build_remote, .live = true};

/* ---- Palanu Link (PalanuOS extras, under Developer > System): the cable trust, pairing, the paired devices, the TCP server ---- */

static void pair_run(void *ctx) {
    (void)ctx;
    settings_pair_open();
}

static void tcp_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    if (!psys_svc_find("link-tcp", &in)) {
        snprintf(out, cap, "not in this build");
        return;
    }
    if (in.state != PSYS_SVC_RUNNING) {
        snprintf(out, cap, "%s", psys_svc_state_name(in.state));
        return;
    }
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    snprintf(out, cap, "%s:7000", st.ip[0] ? st.ip : "running");
}

static void ws_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    psys_svc_info_t in;
    if (!psys_svc_find("link-ws", &in)) {
        snprintf(out, cap, "not in this build");
        return;
    }
    if (in.state != PSYS_SVC_RUNNING) {
        snprintf(out, cap, "%s", psys_svc_state_name(in.state));
        return;
    }
    pal_wifi_status_t st;
    pal_wifi_status(&st);
    snprintf(out, cap, "%s:7001", st.ip[0] ? st.ip : "running");
}

static void connected_text(void *ctx, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "%s", pal_link_connected() ? "yes" : "no");
}

static pal_menu_row_t REMOTE_LINK_STORE[14] NEMA_EXT_BSS;

static void build_remote_link(pal_menu_page_t *p) {
    pal_menu_clear(p);
    pal_menu_row_t r = {.kind = PAL_MENU_SECTION, .label = "Cable"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_TOGGLE, .label = "Trust the cable", .icon = "link",
                         .ctx = (void *)"link.uart.trust", .get = settings_bool_get, .set = settings_bool_set,
                         .hint = "Off: the cable needs a paired token too"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Pairing"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Pair new device", .icon = "link", .run = pair_run,
                         .hint = "Shows a 6-digit code for 120 s"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SUBMENU, .label = "Paired devices", .icon = "link", .text = count_text,
                         .open = settings_open_ctx, .ctx = &PG_LINK_CLIENTS, .hint = "Revoke one, or all below"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_ACTION, .label = "Revoke all devices", .icon = "lock", .danger = true,
                         .run = revoke_all_run, .confirm = "Every paired device must pair again.", .ok_label = "Revoke all"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_SECTION, .label = "Wireless"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Link over BLE", .icon = "link", .text = link_ble_text};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "TCP server", .icon = "wifi", .text = tcp_text,
                         .hint = "Runs while Wi-Fi has an address"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "WebSocket (Web Forge)", .icon = "wifi", .text = ws_text,
                         .hint = "Runs while Wi-Fi has an address"};
    pal_menu_add(p, &r);
    r = (pal_menu_row_t){.kind = PAL_MENU_INFO, .label = "Client connected", .text = connected_text};
    pal_menu_add(p, &r);
}

pal_menu_page_t PG_REMOTE_LINK = {.title = "Palanu Link", .store = REMOTE_LINK_STORE, .cap = 14, .build = build_remote_link,
                                  .live = true};

/* ---- the pairing overlay: the code large, the countdown, closed on Enter, Esc or when the window ends ---- */

static void pair_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full);
static bool pair_key(pal_view_t *v, const pal_key_t *k);
static void pair_tick(pal_view_t *v, uint32_t now);
static void pair_leave(pal_view_t *v);

static const pal_legend_item_t PAIR_LEGENDS[] = {{PAL_ACT_OK, "done", NULL}, {PAL_ACT_BACK, "cancel", NULL}};
static pal_view_t s_pair_view = {.id = "settings-pair", .title = "Pair", .flags = PAL_VIEW_OPAQUE | PAL_VIEW_NOBAR,
                                 .draw = pair_draw, .key = pair_key, .tick = pair_tick, .tick_ms = 1000,
                                 .leave = pair_leave, .legends = PAIR_LEGENDS, .nlegends = 2};
static char s_pair_status[80];

void settings_pair_open(void) {
    if (pal_link_pair_open(s_code) != NEMA_OK) {
        settings_toast("Could not open pairing");
        return;
    }
    (void)pal_link_pair_status(s_pair_status, sizeof(s_pair_status));
    if (pal_desktop_overlay_push(&s_pair_view) != NEMA_OK) pal_link_pair_close();
}

static void pair_draw(pal_view_t *v, pal_canvas_t *c, pal_rect_t a, bool full) {
    (void)v, (void)full;
    const pal_theme_t *t = pal_theme_active();
    pal_fill(c, a.x, a.y, a.w, a.h, PAL_C_PAPER);
    int y = a.y + t->comp.title_bar(c, t, a.y, "Pair new device");
    const pal_font_t *f = &PAL_FONT_SPLEEN_8X16;
    int code_w = (int)strlen(s_code) * f->w * 3;
    pal_ui_big_text(c, f, a.x + (a.w - code_w) / 2, y + 20, s_code, 3, PAL_C_INK);
    const char *hint1 = "Enter this code on the computer";
    const char *hint2 = "within the time shown below.";
    pal_text(c, t->tokens.body, a.x + (a.w - pal_text_w(t->tokens.body, hint1)) / 2, y + 90, hint1, PAL_C_INK,
             PAL_TRANSPARENT);
    pal_text(c, t->tokens.body, a.x + (a.w - pal_text_w(t->tokens.body, hint2)) / 2, y + 90 + f->h, hint2, PAL_C_INK,
             PAL_TRANSPARENT);
    /* the window's own line: "pairing window open: 97 s left, 3 tries left" or "closed" */
    pal_text(c, t->tokens.caption, a.x + 6, y + 140, s_pair_status, PAL_C_DIM, PAL_TRANSPARENT);
}

static bool pair_key(pal_view_t *v, const pal_key_t *k) {
    (void)v;
    if (k->sym != PAL_KEY_ENTER) return false;
    pal_desktop_overlay_pop(); /* leave closes the window */
    return true;
}

static void pair_tick(pal_view_t *v, uint32_t now) {
    (void)v, (void)now;
    (void)pal_link_pair_status(s_pair_status, sizeof(s_pair_status));
    if (strstr(s_pair_status, "closed")) {
        pal_desktop_overlay_pop();
        return;
    }
    pal_desktop_invalidate();
}

static void pair_leave(pal_view_t *v) {
    (void)v;
    pal_link_pair_close();
    settings_toast("Pairing closed");
    pal_menu_refresh(settings_menu());
}
