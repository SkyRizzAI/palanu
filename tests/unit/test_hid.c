/* PalanuOS keys as USB HID keyboard usages (pal/hid.h, ADR-0018): every printable ASCII character has a key, Shift is added where
 * the US layout needs it, Ctrl and Alt pass through, keys with no usage are refused, and a typed key is a press then a release. */
#include <string.h>

#include "nema/nema.h"
#include "pal/hid.h"
#include "test.h"

static bool map(uint8_t sym, uint8_t mods, uint32_t cp, uint8_t *u, uint8_t *m) {
    pal_key_t k = {sym, mods, false, cp};
    return pal_hid_from_key(&k, u, m);
}

TEST(every_printable_character_has_a_key) {
    uint8_t u, m, seen[128] = {0};
    for (uint32_t c = 0x20; c <= 0x7E; c++) {
        CHECK(map(PAL_KEY_CHAR, 0, c, &u, &m));
        CHECK(u >= 0x04 && u <= 0x38);
        seen[u] |= (uint8_t)(1u << ((m & PAL_HID_MOD_LSHIFT) ? 1 : 0));
    }
    CHECK(map(PAL_KEY_CHAR, 0, 'a', &u, &m) && u == 0x04 && m == 0);
    CHECK(map(PAL_KEY_CHAR, 0, 'Z', &u, &m) && u == 0x1D && m == PAL_HID_MOD_LSHIFT);
    CHECK(map(PAL_KEY_CHAR, 0, '0', &u, &m) && u == 0x27 && m == 0);
    CHECK(map(PAL_KEY_CHAR, 0, '!', &u, &m) && u == 0x1E && m == PAL_HID_MOD_LSHIFT);
    CHECK(map(PAL_KEY_CHAR, 0, '@', &u, &m) && u == 0x1F && m == PAL_HID_MOD_LSHIFT);
    CHECK(map(PAL_KEY_CHAR, 0, ' ', &u, &m) && u == 0x2C);
    CHECK(map(PAL_KEY_CHAR, 0, '`', &u, &m) && u == 0x35 && m == 0);
    CHECK(map(PAL_KEY_CHAR, 0, '~', &u, &m) && u == 0x35 && m == PAL_HID_MOD_LSHIFT); /* the last entry of the table */
    CHECK(map(PAL_KEY_CHAR, 0, '|', &u, &m) && u == 0x31 && m == PAL_HID_MOD_LSHIFT);
    CHECK(map(PAL_KEY_CHAR, PAL_MOD_SHIFT, 'A', &u, &m) && m == PAL_HID_MOD_LSHIFT); /* Shift is in the character already */
}

TEST(control_keys_and_modifiers) {
    uint8_t u, m;
    CHECK(map(PAL_KEY_CHAR, PAL_MOD_CTRL, 'c', &u, &m) && u == 0x06 && m == PAL_HID_MOD_LCTRL);
    CHECK(map(PAL_KEY_CHAR, PAL_MOD_ALT, 'x', &u, &m) && u == 0x1B && m == PAL_HID_MOD_LALT);
    CHECK(map(PAL_KEY_UP, PAL_MOD_SHIFT, 0, &u, &m) && u == 0x52 && m == PAL_HID_MOD_LSHIFT);
    CHECK(map(PAL_KEY_ENTER, 0, 0, &u, &m) && u == 0x28);
    CHECK(map(PAL_KEY_BACKSPACE, 0, 0, &u, &m) && u == 0x2A);
    CHECK(map(PAL_KEY_PGDN, 0, 0, &u, &m) && u == 0x4E);
    CHECK(!map(PAL_KEY_CHAR, 0, 0xE9, &u, &m)); /* e acute: no key on a US keyboard */
    CHECK(!map(PAL_KEY_GOHOME, 0, 0, &u, &m));  /* the system's own keys stay here */
    CHECK(!map(PAL_KEY_SWITCH, 0, 0, &u, &m));
}

static pal_hid_state_t g_state;
static uint8_t g_rep[4][7];
static int g_n;
static nema_err_t f_start(const char *n) { (void)n; return NEMA_OK; }
static void f_stop(void) {}
static pal_hid_state_t f_state(void) { return g_state; }
static nema_err_t f_report(uint8_t mods, const uint8_t keys[6]) {
    if (g_n < 4) g_rep[g_n][0] = mods, memcpy(g_rep[g_n] + 1, keys, 6);
    g_n++;
    return NEMA_OK;
}
static const pal_hid_ops_t FAKE = {f_start, f_stop, f_state, f_report, NULL};

TEST(a_typed_key_is_a_press_then_a_release) {
    pal_hid_set(NULL);
    pal_key_t k = {PAL_KEY_CHAR, 0, false, 'Q'};
    CHECK(pal_hid_type(&k) == NEMA_ERR_STATE); /* no transport */
    pal_hid_set(&FAKE);
    g_state = PAL_HID_WAITING;
    CHECK(pal_hid_type(&k) == NEMA_ERR_STATE); /* not paired yet: nothing sent */
    CHECK_EQ(g_n, 0);
    g_state = PAL_HID_CONNECTED;
    CHECK(pal_hid_type(&k) == NEMA_OK);
    CHECK_EQ(g_n, 2);
    CHECK(g_rep[0][0] == PAL_HID_MOD_LSHIFT && g_rep[0][1] == 0x14);
    uint8_t zero[7] = {0};
    CHECK(!memcmp(g_rep[1], zero, 7));
    pal_key_t none = {PAL_KEY_CHAR, 0, false, 0x4E2D};
    CHECK(pal_hid_type(&none) == NEMA_ERR_INVAL);
    CHECK_EQ(g_n, 2);
}

static pal_hid_state_t g_usb_state;
static pal_hid_state_t f_usb_state(void) { return g_usb_state; }
static const pal_hid_ops_t USB = {f_start, f_stop, f_usb_state, f_report, NULL};

TEST(a_connected_usb_cable_wins_over_bluetooth) {
    pal_hid_set(&FAKE);
    pal_hid_set_transport(PAL_HID_USB, &USB);
    g_usb_state = PAL_HID_WAITING;
    CHECK(pal_hid() == &FAKE); /* the cable is not plugged in: Bluetooth */
    g_usb_state = PAL_HID_CONNECTED;
    CHECK(pal_hid() == &USB);
    pal_hid_set(NULL);
    g_usb_state = PAL_HID_WAITING;
    CHECK(pal_hid() == &USB); /* only USB: it is the one to start */
    CHECK(pal_hid_transport(PAL_HID_BLE) == NULL);
    pal_hid_set_transport(PAL_HID_USB, NULL);
}

int main(void) {
    RUN(every_printable_character_has_a_key);
    RUN(control_keys_and_modifiers);
    RUN(a_typed_key_is_a_press_then_a_release);
    RUN(a_connected_usb_cable_wins_over_bluetooth);
    return TEST_DONE();
}
