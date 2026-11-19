/* LoRa: the settings, the check, the time on air, and send and receive through the board's module (pal/lora.h). */
#include "pal/lora.h"

#include "pal/settings.h"

const pal_device_t *pal_lora_device(void) {
    for (int i = 0; i < pal_dev_count(PAL_DEV_RADIO); i++) {
        const pal_device_t *d = pal_dev_get(PAL_DEV_RADIO, i);
        if (d && d->radio.type == PAL_DEVRADIO_LORA && d->radio.lora) return d;
    }
    return NULL;
}

void pal_lora_cfg_load(pal_lora_cfg_t *c) {
    c->freq_hz = (uint32_t)psys_settings_int("lora.freq", 868100000);
    c->sf = (uint8_t)psys_settings_int("lora.sf", 9);
    c->bw_khz = (uint16_t)psys_settings_int("lora.bw", 125);
    c->cr = (uint8_t)psys_settings_int("lora.cr", 5);
    c->power_dbm = (int8_t)psys_settings_int("lora.power", 14);
    c->sync = 0x12; /* private networks: PalanuOS devices hear each other, LoRaWAN gateways do not */
    c->preamble = 8;
}

const char *pal_lora_cfg_check(const pal_lora_cfg_t *c) {
    if (c->freq_hz < 150000000u || c->freq_hz > 960000000u) return "frequency outside 150 to 960 MHz";
    if (c->sf < 5 || c->sf > 12) return "spreading factor is 5 to 12";
    if (c->bw_khz != 31 && c->bw_khz != 62 && c->bw_khz != 125 && c->bw_khz != 250 && c->bw_khz != 500) return "bandwidth is 125, 250 or 500 kHz";
    if (c->cr < 5 || c->cr > 8) return "coding rate is 5 to 8 (4/5 to 4/8)";
    if (c->power_dbm < -9 || c->power_dbm > 22) return "power is -9 to 22 dBm";
    return NULL;
}

uint32_t pal_lora_airtime_ms(const pal_lora_cfg_t *c, size_t n) {
    uint32_t tsym_us = (uint32_t)((1000u << c->sf) / c->bw_khz); /* 2^SF / BW */
    int de = tsym_us > 16000u ? 1 : 0;                           /* low data rate optimisation above 16 ms a symbol */
    int num = 8 * (int)n - 4 * c->sf + 28 + 16;                  /* CRC on, explicit header */
    int den = 4 * (c->sf - 2 * de);
    int blocks = num > 0 ? (num + den - 1) / den : 0;
    uint32_t symbols = 8u + (uint32_t)blocks * c->cr;
    uint32_t us = (uint32_t)c->preamble * tsym_us + tsym_us * 17u / 4u + symbols * tsym_us; /* preamble + 4.25 + payload */
    return (us + 999u) / 1000u;
}

static const pal_device_t *ready(nema_err_t *err) {
    const pal_device_t *d = pal_lora_device();
    if (!d) {
        *err = NEMA_ERR_NOENT;
        return NULL;
    }
    pal_lora_cfg_t c;
    pal_lora_cfg_load(&c);
    *err = pal_lora_cfg_check(&c) ? NEMA_ERR_INVAL : d->radio.lora->configure(d->ctx, &c);
    return *err == NEMA_OK ? d : NULL;
}

nema_err_t pal_lora_send(const uint8_t *buf, size_t n) {
    if (n == 0 || n > PAL_LORA_MAX) return NEMA_ERR_INVAL;
    nema_err_t e;
    const pal_device_t *d = ready(&e);
    if (!d) return e;
    pal_lora_cfg_t c;
    pal_lora_cfg_load(&c);
    return d->radio.lora->send(d->ctx, buf, n, (int32_t)pal_lora_airtime_ms(&c, n) * 2 + 500);
}

int pal_lora_recv(uint8_t *buf, size_t cap, int32_t timeout_ms, pal_lora_meta_t *meta) {
    nema_err_t e;
    const pal_device_t *d = ready(&e);
    if (!d) return e;
    return d->radio.lora->recv(d->ctx, buf, cap, timeout_ms, meta);
}
