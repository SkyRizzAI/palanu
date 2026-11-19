/* LoRa radio modules (ADR-0020): a board declares a module as a PAL_DEV_RADIO device of type PAL_DEVRADIO_LORA with its driver's
 * ops (the SX126x driver, pal/sx126x.h; the simulator's module). The system keeps the settings (lora.*) and offers send and
 * receive to the lora command and to apps. Point to point: raw LoRa packets, no LoRaWAN, no mesh. */
#ifndef PAL_LORA_H
#define PAL_LORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nema/types.h"
#include "pal/device.h"

#define PAL_LORA_MAX 255 /* bytes in one packet */

typedef struct {
    uint32_t freq_hz;  /* 868100000 (EU868), 915000000 (US915), 923200000 (AS923)... the owner's region decides */
    uint8_t sf;        /* spreading factor 5..12 */
    uint16_t bw_khz;   /* 125, 250 or 500 (also 62 and 31 on chips that have them) */
    uint8_t cr;        /* coding rate 4/5..4/8, written 5..8 */
    int8_t power_dbm;  /* transmit power */
    uint8_t sync;      /* sync word: 0x12 private, 0x34 public */
    uint16_t preamble; /* symbols */
} pal_lora_cfg_t;

typedef struct {
    int16_t rssi_dbm;
    int8_t snr_db;
} pal_lora_meta_t;

typedef struct pal_lora_ops {
    nema_err_t (*configure)(void *ctx, const pal_lora_cfg_t *c);
    /* Sends one packet; blocks until it is on air or timeout_ms passes. */
    nema_err_t (*send)(void *ctx, const uint8_t *buf, size_t n, int32_t timeout_ms);
    /* Waits up to timeout_ms for one packet: its length, 0 when none came, negative on an error (a bad CRC is NEMA_ERR_PROTO). */
    int (*recv)(void *ctx, uint8_t *buf, size_t cap, int32_t timeout_ms, pal_lora_meta_t *meta);
} pal_lora_ops_t;

/* The board's first LoRa module, or NULL. */
const pal_device_t *pal_lora_device(void);
/* The configuration in the settings (lora.freq, lora.sf, lora.bw, lora.cr, lora.power), checked. */
void pal_lora_cfg_load(pal_lora_cfg_t *c);
/* NULL when c is acceptable, else why not. */
const char *pal_lora_cfg_check(const pal_lora_cfg_t *c);
/* Configure the module from the settings, then send or receive. NEMA_ERR_NOENT without a module. */
nema_err_t pal_lora_send(const uint8_t *buf, size_t n);
int pal_lora_recv(uint8_t *buf, size_t cap, int32_t timeout_ms, pal_lora_meta_t *meta);
/* Time on air of a packet of n bytes in milliseconds (the Semtech formula, explicit header, CRC on). */
uint32_t pal_lora_airtime_ms(const pal_lora_cfg_t *c, size_t n);

#endif
