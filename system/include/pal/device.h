/* Board devices: the list a board declares in pal_board_desc_t.devices (docs/adr/0005-board-device-list.md). One entry is
 * one physical device. Apps and the system find them by kind or id, never by board name, so a board with five LEDs shows and
 * tests five. Hooks are optional: a NULL hook means the device cannot do that. */
#ifndef PAL_DEVICE_H
#define PAL_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAL_DEV_LED = 1, /* one LED or a strip of them */
    PAL_DEV_BUZZER,  /* a buzzer or speaker that plays a tone */
    PAL_DEV_BATTERY, /* a battery or supply that can be measured */
    PAL_DEV_STORAGE, /* flash, a card slot: where files live */
    PAL_DEV_SENSOR,  /* one measured quantity (temperature, light, motion axis) */
    PAL_DEV_GPIO,    /* one pin */
    PAL_DEV_BUS,     /* an I2C, SPI or UART bus */
    PAL_DEV_CAMERA,  /* a camera (declared so the system knows; no API yet) */
    PAL_DEV_MIC,     /* a microphone */
    PAL_DEV_TOUCH,   /* a touch panel */
    PAL_DEV_RADIO,   /* a Wi-Fi, Bluetooth LE or LoRa radio */
} pal_dev_kind_t;

typedef enum { PAL_BUS_I2C = 1, PAL_BUS_SPI, PAL_BUS_UART } pal_bus_type_t;
typedef enum { PAL_DEVRADIO_WIFI = 1, PAL_DEVRADIO_BLE, PAL_DEVRADIO_LORA } pal_devradio_type_t;
struct pal_lora_ops; /* pal/lora.h */

typedef struct pal_device {
    const char *id;   /* stable, lower case, unique on the board: "led0", "status", "strip" */
    const char *name; /* for screens: "Status LED" */
    pal_dev_kind_t kind;
    void *ctx;        /* the driver's own state, passed back to every hook */
    union {
        struct {
            int pixels; /* how many LEDs this device drives (1 for a single LED) */
            int colors; /* 1: brightness only (the largest of r, g, b is used); 3: RGB */
            void (*set)(void *ctx, int pixel, uint8_t r, uint8_t g, uint8_t b); /* sets one pixel and shows it */
        } led;
        struct {
            void (*beep)(void *ctx, int freq_hz, int ms);
        } buzzer;
        struct {
            int (*percent)(void *ctx); /* 0..100, -1 when unknown; may be NULL */
            int (*mv)(void *ctx);      /* supply millivolts, -1 when unknown; may be NULL */
        } battery;
        struct {
            bool removable;                 /* a card slot (true) or soldered flash (false) */
            bool (*present)(void *ctx);     /* a card is in; NULL: always present */
        } storage;
        struct {
            const char *unit;                          /* "mC" is not used: milli-units of this unit: "C", "lux", "g" */
            int (*read)(void *ctx, int32_t *milli);    /* the value times 1000; 0 on success, negative on failure */
        } sensor;
        struct {
            int pin;                                     /* the chip pin number, for display */
            bool app;                                    /* true: apps may drive it (the board promises it is free) */
            int (*read)(void *ctx);                      /* 0 or 1, negative on failure; NULL: cannot read */
            int (*write)(void *ctx, int level);          /* 0 on success; NULL: input only */
        } gpio;
        struct {
            pal_bus_type_t type;
            bool app;                                    /* true: apps may use it */
            int (*probe)(void *ctx, int addr);           /* I2C: 1 when addr answers, 0 when not, negative on failure; may be NULL */
            /* One transfer: write tx (txn bytes) then read rx (rxn bytes). addr is the I2C 7-bit address, the SPI chip-select
             * index, and ignored for UART. Returns 0, or negative on failure; may be NULL. */
            int (*xfer)(void *ctx, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn);
        } bus;
        struct { int present; } camera, mic, touch; /* info only: these declare that the hardware exists */
        struct {
            pal_devradio_type_t type;
            const struct pal_lora_ops *lora; /* PAL_DEVRADIO_LORA: the module's driver (pal/lora.h), its state in ctx */
        } radio;
    };
} pal_device_t;

/* How many devices of this kind the running board has. */
int pal_dev_count(pal_dev_kind_t kind);
/* The n-th device of this kind (0 based), or NULL. */
const pal_device_t *pal_dev_get(pal_dev_kind_t kind, int n);
/* The device with this id, or NULL. */
const pal_device_t *pal_dev_find(const char *id);

/* LED: pixel -1 sets every pixel of the device. Out of range pixels and non-LED devices do nothing. */
void pal_led_set(const pal_device_t *d, int pixel, uint8_t r, uint8_t g, uint8_t b);
/* Every LED of every LED device (what pal_rgb does). */
void pal_led_all(uint8_t r, uint8_t g, uint8_t b);
/* GPIO: read a level, or write one (an app-visible check is the caller's: see pal_dev_app_visible). */
int pal_gpio_read(const pal_device_t *d);
int pal_gpio_write(const pal_device_t *d, int level);
/* Sensor: the value times 1000 in *milli; 0 on success. */
int pal_sensor_read(const pal_device_t *d, int32_t *milli);
/* Battery: percent and millivolts of this device, -1 when unknown. */
int pal_dev_battery_percent(const pal_device_t *d);
int pal_dev_battery_mv(const pal_device_t *d);
/* I2C probe on a bus device: 1 answers, 0 silent, negative on failure. */
int pal_bus_probe(const pal_device_t *d, int addr);
/* One bus transfer (see the bus.xfer hook); -1 when the device is no bus or has no xfer. */
int pal_bus_xfer(const pal_device_t *d, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn);
/* What kind of devices an app may drive: LED, buzzer, sensors and batteries always; a pin or bus only when the board marked it app. */
bool pal_dev_app_visible(const pal_device_t *d);
/* Short name of a kind: "led", "buzzer", "battery", "storage", "sensor", "gpio", "bus". */
const char *pal_dev_kind_name(pal_dev_kind_t kind);
/* Buzzer: plays a tone on this device. */
void pal_dev_beep(const pal_device_t *d, int freq_hz, int ms);

#endif
