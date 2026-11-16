/* The simulated hardware shared by the native and the browser simulator (platform/host/sim_hw.c): the device table it builds
 * and the state the page changes while the system runs. */
#include <string.h>

#include "../../platform/host/sim_hw.h"
#include "nema/nema.h"
#include "pal/board.h"
#include "test.h"

static const pal_device_t *find(const pal_board_desc_t *b, const char *id) {
    for (int i = 0; i < b->ndevices; i++)
        if (strcmp(b->devices[i].id, id) == 0) return &b->devices[i];
    return NULL;
}

TEST(device_table_follows_the_config) {
    static pal_board_desc_t b;
    sim_hw_cfg_t cfg = {.leds = 2, .sensors = 1, .gpios = 3, .i2c = true, .radios = true};
    sim_hw_init(&b, &cfg);
    CHECK_EQ(b.ndevices, 1 + 2 + 1 + 3 + 1 + 2);
    CHECK(find(&b, "led1") != NULL);
    CHECK(find(&b, "temp0") != NULL);
    CHECK(find(&b, "gpio2") != NULL);
    CHECK(find(&b, "i2c0") != NULL);
    CHECK(find(&b, "wifi0") != NULL && find(&b, "ble0") != NULL);
    CHECK(b.devices[b.ndevices - 1].radio.type == PAL_DEVRADIO_BLE);
}

TEST(nothing_asked_leaves_the_table_alone) {
    static pal_board_desc_t b;
    sim_hw_cfg_t cfg = {0};
    sim_hw_init(&b, &cfg);
    CHECK_EQ(b.ndevices, 0);
}

TEST(sensor_pin_battery_and_i2c_state) {
    static pal_board_desc_t b;
    sim_hw_cfg_t cfg = {.sensors = 2, .gpios = 1, .i2c = true};
    sim_hw_init(&b, &cfg);
    int32_t milli = 0;
    const pal_device_t *t1 = &b.devices[2];
    CHECK_EQ(t1->sensor.read(t1->ctx, &milli), 0);
    CHECK_EQ(milli, 22500); /* the default reading: 21.5 C plus one degree per index */
    sim_hw_sensor_set(1, -5000);
    t1->sensor.read(t1->ctx, &milli);
    CHECK_EQ(milli, -5000);
    sim_hw_gpio_set(0, 1);
    CHECK_EQ(sim_hw_gpio_get(0), 1);
    sim_hw_set_battery(55);
    CHECK_EQ(sim_hw_battery(), 55);
    sim_hw_set_battery(-1);
    CHECK_EQ(sim_hw_battery(), -1);
    const pal_device_t *bus = &b.devices[b.ndevices - 1];
    CHECK(bus->bus.probe(bus->ctx, 0x48) && bus->bus.probe(bus->ctx, 0x68));
    sim_hw_i2c_set(0x48, false);
    sim_hw_i2c_set(0x3c, true);
    CHECK(!bus->bus.probe(bus->ctx, 0x48));
    CHECK(bus->bus.probe(bus->ctx, 0x3c));
}

TEST(wifi_networks_passwords_and_refusal) {
    const pal_wifi_ops_t *w = sim_hw_wifi_ops();
    pal_ap_t aps[8];
    size_t n = 0;
    CHECK_EQ(w->scan(aps, 8, &n), NEMA_OK);
    CHECK_EQ(n, 4); /* the defaults */
    sim_hw_ap_clear();
    CHECK_EQ(sim_hw_ap_add("Lab", -50, 6, true, "s3cret"), 0);
    CHECK_EQ(sim_hw_ap_add("Open", -60, 1, false, ""), 1);
    w->scan(aps, 8, &n);
    CHECK_EQ(n, 2);
    CHECK(strcmp(aps[0].ssid, "Lab") == 0 && aps[0].secure);
    CHECK(w->connect("Lab", "wrong", 1000) != NEMA_OK);
    CHECK(!sim_hw_wifi_connected());
    CHECK_EQ(w->connect("Lab", "s3cret", 1000), NEMA_OK);
    CHECK(sim_hw_wifi_connected());
    pal_wifi_status_t st;
    w->status(&st);
    CHECK(st.connected && strcmp(st.ssid, "Lab") == 0 && st.rssi == -50);
    sim_hw_wifi_set_connected(false);
    CHECK(!sim_hw_wifi_connected());
    CHECK_EQ(w->connect("Open", "anything", 1000), NEMA_OK);
    sim_hw_wifi_config("10.0.0.7", true);
    CHECK(w->connect("Open", "", 1000) != NEMA_OK);
    sim_hw_wifi_config("10.0.0.7", false);
    CHECK_EQ(w->connect("Open", "", 1000), NEMA_OK);
    w->status(&st);
    CHECK(strcmp(st.ip, "10.0.0.7") == 0);
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(device_table_follows_the_config);
    RUN(nothing_asked_leaves_the_table_alone);
    RUN(sensor_pin_battery_and_i2c_state);
    RUN(wifi_networks_passwords_and_refusal);
    return TEST_DONE();
}
