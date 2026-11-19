/* The Semtech SX1261/SX1262/SX1268 LoRa transceiver (ADR-0020), portable: it talks through the board's SPI bus device and pin
 * devices. A board with the chip declares a PAL_DEV_RADIO device {.type = PAL_DEVRADIO_LORA, .lora = &PAL_SX126X_OPS} whose ctx is
 * a pal_sx126x_t naming those devices. */
#ifndef PAL_SX126X_H
#define PAL_SX126X_H

#include <stdbool.h>
#include <stdint.h>

#include "pal/device.h"
#include "pal/lora.h"

typedef struct {
    const pal_device_t *spi;   /* PAL_DEV_BUS of type SPI */
    int cs;                    /* its chip-select index for this chip */
    const pal_device_t *busy;  /* PAL_DEV_GPIO: BUSY (high while the chip works) */
    const pal_device_t *reset; /* PAL_DEV_GPIO: NRESET, or NULL */
    uint16_t tcxo_mv;          /* 0: a crystal; otherwise DIO3 powers a TCXO at this voltage (1600 to 3300) */
    bool dio2_rf_switch;       /* DIO2 drives the antenna switch (most modules) */
    bool low_power_pa;         /* an SX1261 (15 dBm PA) rather than an SX1262 or SX1268 */
    /* the driver's own */
    bool started;
    pal_lora_cfg_t cfg;
} pal_sx126x_t;

extern const pal_lora_ops_t PAL_SX126X_OPS;

/* The value of the RF frequency register for freq_hz (32 MHz crystal): freq * 2^25 / 32 MHz. */
uint32_t pal_sx126x_freq_reg(uint32_t freq_hz);

#endif
