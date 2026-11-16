/* Board devices (API level 3, pins and sensors from 4, bus transfers from 5): the LEDs, buzzers, sensors, batteries and pins the board declares, found at run time so one app runs on a board with one
 * LED and on a board with five. A device has an index (0 to pal_dev_total() - 1; stable while the board runs), an id such as
 * "led0", a kind, and a name. Needs the dev permission in the manifest. The same devices are on the terminal: the dev command. */
#ifndef PALANU_API_DEV_H
#define PALANU_API_DEV_H

#include "annot.h"

PAL_SINCE(3) PAL_CAP("dev") PAL_WASM(dev_total)
PAL_DOC("How many devices the board declares (a pin or bus the board did not open to apps still counts here but answers PAL_ERR_NOENT); 0 when it has none.")
int32_t pal_dev_total(void);

PAL_SINCE(3) PAL_CAP("dev") PAL_WASM(dev_info)
PAL_DOC("Writes the text 'id kind name' of device index, separated by tabs, kind being led, buzzer, battery, sensor, gpio or bus, then for a led a tab, its pixel count, a tab and its colour count (3 RGB, 1 brightness only); for a sensor a tab and its unit; for a gpio a tab, its pin and rw or r; for a bus a tab and i2c, spi or uart; NUL-terminated, into out. Returns the length without the NUL, PAL_ERR_NOENT for an index out of range, or PAL_ERR_INVAL when out is too small.")
int32_t pal_dev_info(int32_t index, PAL_OUT(len) void *out, uint32_t len);

PAL_SINCE(3) PAL_CAP("dev") PAL_WASM(dev_lookup)
PAL_DOC("The index of the device whose id is id (NUL-terminated), or PAL_ERR_NOENT.")
int32_t pal_dev_lookup(const char *id);

PAL_SINCE(3) PAL_CAP("dev") PAL_WASM(dev_set_led)
PAL_DOC("Sets one pixel of LED device index to rgb (0xRRGGBB); pixel -1 sets every pixel of it. A brightness-only LED uses the brightest channel. Returns 0, PAL_ERR_NOENT (no such device), or PAL_ERR_INVAL (not an LED, or the pixel is out of range).")
int32_t pal_dev_set_led(int32_t index, int32_t pixel, uint32_t rgb);

PAL_SINCE(3) PAL_CAP("dev") PAL_WASM(dev_play_tone)
PAL_DOC("Plays a tone of freq_hz (20 to 20000) for ms milliseconds (1 to 5000) on buzzer device index. Returns 0, PAL_ERR_NOENT (no such device), or PAL_ERR_INVAL (not a buzzer, or a value out of range).")
int32_t pal_dev_play_tone(int32_t index, int32_t freq_hz, int32_t ms);

PAL_SINCE(4) PAL_CAP("dev") PAL_WASM(dev_gpio_read)
PAL_DOC("Reads the level (0 or 1) of gpio device index. Returns the level, PAL_ERR_NOENT (no such device, or a pin the board did not open to apps), PAL_ERR_INVAL (not a readable pin) or PAL_ERR_IO.")
int32_t pal_dev_gpio_read(int32_t index);

PAL_SINCE(4) PAL_CAP("dev") PAL_WASM(dev_gpio_write)
PAL_DOC("Drives gpio device index to level (0 or non-zero). Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not an output pin) or PAL_ERR_IO.")
int32_t pal_dev_gpio_write(int32_t index, int32_t level);

PAL_SINCE(4) PAL_CAP("dev") PAL_WASM(dev_sensor_read)
PAL_DOC("Reads sensor device index into out (len must be at least 4): one little-endian int32, the value times 1000 in the unit pal_dev_info reports (21500 is 21.5). Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not a sensor) or PAL_ERR_IO.")
int32_t pal_dev_sensor_read(int32_t index, PAL_OUT(len) void *out, uint32_t len);

PAL_SINCE(4) PAL_CAP("dev") PAL_WASM(dev_read_battery)
PAL_DOC("The charge of battery device index, 0 to 100, or -1 when the board cannot tell. Returns PAL_ERR_NOENT or PAL_ERR_INVAL (not a battery) otherwise.")
int32_t pal_dev_read_battery(int32_t index);

PAL_SINCE(4) PAL_CAP("dev") PAL_WASM(dev_i2c_probe)
PAL_DOC("Asks I2C bus device index whether a device answers at 7-bit address addr (3 to 119). Returns 1 when it does, 0 when not, PAL_ERR_NOENT, PAL_ERR_INVAL (not an I2C bus or a bad address) or PAL_ERR_IO.")
int32_t pal_dev_i2c_probe(int32_t index, int32_t addr);

PAL_SINCE(5) PAL_CAP("dev") PAL_WASM(dev_bus_xfer)
PAL_DOC("One transfer on bus device index (I2C, SPI or UART): writes tx (txn bytes, may be 0) then reads rxn bytes into rx (may be 0). addr is the I2C 7-bit address or the SPI chip-select index, and ignored on UART. Returns 0, PAL_ERR_NOENT, PAL_ERR_INVAL (not a bus, no transfer, or more than 256 bytes) or PAL_ERR_IO.")
int32_t pal_dev_bus_xfer(int32_t index, int32_t addr, PAL_IN(txn) const void *tx, uint32_t txn, PAL_OUT(rxn) void *rx, uint32_t rxn);

#endif
