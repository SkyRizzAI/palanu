/* Saved Wi-Fi networks (pal/wifi.h): the most recent first, the oldest leaves a full list, Auto-Join picks in order, forget,
 * manual addresses, the list survives through the board's store, and dotted addresses parse strictly. */
#include <string.h>

#include "nema/nema.h"
#include "pal/wifi.h"
#include "test.h"

static uint8_t g_store[2048];
static size_t g_len;
static int g_disconnects;
static char g_current[33];

static nema_err_t st(const void *b, size_t n) {
    memcpy(g_store, b, n);
    g_len = n;
    return NEMA_OK;
}
static size_t ld(void *b, size_t cap) {
    size_t n = g_len < cap ? g_len : cap;
    memcpy(b, g_store, n);
    return n;
}
static nema_err_t disc(void) {
    g_disconnects++;
    return NEMA_OK;
}
static void status(pal_wifi_status_t *s) {
    s->connected = g_current[0] != 0;
    strcpy(s->ssid, g_current);
}
static nema_err_t forget(void) { return NEMA_OK; }
static const pal_wifi_ops_t OPS = {.disconnect = disc, .status = status, .forget = forget, .store = st, .load = ld};

static void fresh(void) {
    g_len = 0;
    pal_wifi_set_ops(&OPS);
}

TEST(a_join_saves_the_network_first_and_keeps_its_settings) {
    fresh();
    CHECK(!pal_wifi_has_credentials());
    pal_wifi_joined("Home", "secret1");
    pal_wifi_joined("Office", "");
    pal_wifi_net_t n[PAL_WIFI_NETS];
    CHECK_EQ((int)pal_wifi_saved(n, PAL_WIFI_NETS), 2);
    CHECK(!strcmp(n[0].ssid, "Office") && !n[0].secure && n[0].auto_join);
    CHECK(!strcmp(n[1].ssid, "Home") && n[1].secure);
    CHECK(pal_wifi_set_auto("Home", false) == NEMA_OK);
    pal_wifi_joined("Home", "secret2"); /* joined again: first, Auto-Join stays off, the new password kept */
    CHECK_EQ((int)pal_wifi_saved(n, PAL_WIFI_NETS), 2);
    CHECK(!strcmp(n[0].ssid, "Home") && !n[0].auto_join);
    char s[33], p[65];
    CHECK(pal_wifi_pick(0, s, p) && !strcmp(s, "Office")); /* Home does not join by itself */
    CHECK(!pal_wifi_pick(1, s, p));
    CHECK(pal_wifi_set_auto("Home", true) == NEMA_OK);
    CHECK(pal_wifi_pick(0, s, p) && !strcmp(s, "Home") && !strcmp(p, "secret2"));
    CHECK(pal_wifi_has_credentials());
}

TEST(a_full_list_drops_the_least_recently_used) {
    fresh();
    char name[8];
    for (int i = 0; i < PAL_WIFI_NETS + 1; i++) {
        name[0] = 'N', name[1] = (char)('0' + i), name[2] = 0;
        pal_wifi_joined(name, "pw");
    }
    pal_wifi_net_t n[PAL_WIFI_NETS];
    CHECK_EQ((int)pal_wifi_saved(n, PAL_WIFI_NETS), PAL_WIFI_NETS);
    CHECK(!strcmp(n[0].ssid, "N8"));
    CHECK(!pal_wifi_saved_get("N0", NULL)); /* the oldest went */
    CHECK(pal_wifi_saved_get("N1", NULL));
}

TEST(forget_removes_one_and_disconnects_only_the_one_in_use) {
    fresh();
    pal_wifi_joined("A", "1");
    pal_wifi_joined("B", "2");
    strcpy(g_current, "B");
    g_disconnects = 0;
    CHECK(pal_wifi_forget_net("A") == NEMA_OK);
    CHECK_EQ(g_disconnects, 0);
    CHECK(pal_wifi_forget_net("B") == NEMA_OK);
    CHECK_EQ(g_disconnects, 1);
    CHECK(pal_wifi_forget_net("B") == NEMA_ERR_NOENT);
    CHECK(!pal_wifi_has_credentials());
    g_current[0] = 0;
}

TEST(a_manual_address_is_kept_per_network_and_survives_a_restart) {
    fresh();
    pal_wifi_joined("Lab", "pw");
    pal_ipv4_conf_t c;
    CHECK(pal_ipv4_parse("192.168.1.50", c.ip) && pal_ipv4_parse("255.255.255.0", c.mask) && pal_ipv4_parse("192.168.1.1", c.gw) &&
          pal_ipv4_parse("1.1.1.1", c.dns));
    CHECK(pal_wifi_set_ip("Lab", true, &c) == NEMA_OK);
    CHECK(pal_wifi_set_ip("Nope", true, &c) == NEMA_ERR_NOENT);
    pal_ipv4_conf_t zero = {{0}};
    CHECK(pal_wifi_set_ip("Lab", true, &zero) == NEMA_ERR_INVAL);
    pal_wifi_set_ops(&OPS); /* a restart: read again from the store */
    pal_wifi_net_t n;
    CHECK(pal_wifi_saved_get("Lab", &n) && n.manual_ip);
    char t[16];
    pal_ipv4_text(n.ip.gw, t, sizeof(t));
    CHECK(!strcmp(t, "192.168.1.1"));
    CHECK(pal_wifi_set_ip("Lab", false, NULL) == NEMA_OK);
    CHECK(pal_wifi_saved_get("Lab", &n) && !n.manual_ip);
}

TEST(forget_all_empties_the_list) {
    fresh();
    pal_wifi_joined("X", "1");
    CHECK(pal_wifi_forget() == NEMA_OK);
    CHECK(!pal_wifi_has_credentials());
}

TEST(dotted_addresses_parse_strictly) {
    uint8_t ip[4];
    CHECK(pal_ipv4_parse("10.0.0.255", ip) && ip[0] == 10 && ip[3] == 255);
    CHECK(!pal_ipv4_parse("10.0.0", ip));
    CHECK(!pal_ipv4_parse("10.0.0.256", ip));
    CHECK(!pal_ipv4_parse("10.0.0.1.2", ip));
    CHECK(!pal_ipv4_parse("a.b.c.d", ip));
    CHECK(!pal_ipv4_parse("1.2.3.4 ", ip));
    CHECK(!pal_ipv4_parse("", ip));
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(a_join_saves_the_network_first_and_keeps_its_settings);
    RUN(a_full_list_drops_the_least_recently_used);
    RUN(forget_removes_one_and_disconnects_only_the_one_in_use);
    RUN(a_manual_address_is_kept_per_network_and_survives_a_restart);
    RUN(forget_all_empties_the_list);
    RUN(dotted_addresses_parse_strictly);
    return TEST_DONE();
}
