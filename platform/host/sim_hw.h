/* Simulated hardware shared by the native simulator (sim_main.c) and the browser simulator (platform/web).
 * It builds the board's device table (LEDs, sensors, pins, an I2C bus, radios), the simulated Wi-Fi, and keeps the state
 * that a test or the simulator page can read and change while the system runs. Host-only; never part of a board. */
#ifndef SIM_HW_H
#define SIM_HW_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/board.h"
#include "pal/radio.h"
#include "pal/wifi.h"

#define SIM_HW_MAX_LEDS 8
#define SIM_HW_MAX_EXTRA 8
#define SIM_HW_MAX_APS 16
#define SIM_HW_MAX_I2C 8

typedef struct {
    int leds, sensors, gpios; /* how many of each (0 = none) */
    bool i2c;                 /* an I2C bus with the devices added by sim_hw_i2c_set (default 0x48 and 0x68) */
    bool radios;              /* declare the Wi-Fi and Bluetooth LE radios in the device table */
    bool log;                 /* print LED and pin writes on stderr */
    int lora_port, lora_peer; /* a LoRa module over UDP on 127.0.0.1: listen on lora_port, send to lora_peer (0 = none) */
} sim_hw_cfg_t;

/* Fills board->devices. With nothing asked for the table is left as the board had it. */
void sim_hw_init(pal_board_desc_t *board, const sim_hw_cfg_t *cfg);

/* Battery: percent, or -1 for "cannot measure". */
int sim_hw_battery(void);
void sim_hw_set_battery(int percent);

/* LEDs, sensors, pins: what the system wrote, and what the outside world holds. Out of range indexes are ignored. */
void sim_hw_led_get(int i, uint8_t rgb[3]);
void sim_hw_sensor_set(int i, int32_t milli); /* the next reads return this */
int sim_hw_gpio_get(int i);
void sim_hw_gpio_set(int i, int level); /* an external signal on the pin; a write by the system also lands here */
void sim_hw_i2c_set(int addr, bool present);

/* Wi-Fi: the networks a scan sees (the defaults are four fixed ones), and how connecting behaves. A secure network with a
 * password refuses any other password; an empty password accepts every one. */
const pal_wifi_ops_t *sim_hw_wifi_ops(void);
const pal_radio_ops_t *sim_hw_radio_ops(void);
void sim_hw_ap_clear(void);
int sim_hw_ap_add(const char *ssid, int rssi, int channel, bool secure, const char *password);
void sim_hw_wifi_config(const char *ip, bool refuse_connect);
bool sim_hw_wifi_connected(void);
void sim_hw_wifi_set_connected(bool on); /* an outside drop of the link */
int sim_hw_wifi_rssi(void);

/* Bluetooth LE: the nearby devices a scan would show (kept for the page and apps that scan), and the advertised name. */
void sim_hw_ble_set_name(const char *name);

/* The simulated keyboard transport: reports sent so far, and the last one (modifiers, then six keys). */
uint32_t sim_hw_hid_reports(uint8_t last[7]);

#endif
