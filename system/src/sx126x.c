/* SX126x LoRa driver (pal/sx126x.h), from the datasheet's command set. Every command waits for BUSY low first; reads send the
 * opcode and the status byte's NOP, then read, which the bus hook's write-then-read gives in one chip-select. */
#include "pal/sx126x.h"

#include <string.h>

#include "nema/nema.h"

enum {
    OP_SET_STANDBY = 0x80, OP_SET_PACKET_TYPE = 0x8A, OP_SET_RF_FREQ = 0x86, OP_SET_PA_CONFIG = 0x95, OP_SET_TX_PARAMS = 0x8E,
    OP_SET_MOD_PARAMS = 0x8B, OP_SET_PKT_PARAMS = 0x8C, OP_SET_BUF_BASE = 0x8F, OP_WRITE_BUF = 0x0E, OP_READ_BUF = 0x1E,
    OP_WRITE_REG = 0x0D, OP_SET_DIO_IRQ = 0x08, OP_GET_IRQ = 0x12, OP_CLR_IRQ = 0x02, OP_SET_TX = 0x83, OP_SET_RX = 0x82,
    OP_GET_RX_BUF = 0x13, OP_GET_PKT_STATUS = 0x14, OP_SET_REGULATOR = 0x96, OP_DIO2_RF_SWITCH = 0x9D, OP_DIO3_TCXO = 0x97,
    OP_CALIBRATE = 0x89, OP_CALIBRATE_IMAGE = 0x98,
};
enum { IRQ_TX_DONE = 0x0001, IRQ_RX_DONE = 0x0002, IRQ_HEADER_ERR = 0x0020, IRQ_CRC_ERR = 0x0040, IRQ_TIMEOUT = 0x0200 };
#define REG_SYNC_WORD 0x0740

uint32_t pal_sx126x_freq_reg(uint32_t freq_hz) { return (uint32_t)(((uint64_t)freq_hz << 25) / 32000000u); }

static nema_err_t wait_busy(pal_sx126x_t *x) {
    for (int i = 0; i < 200; i++) { /* 200 ms: a calibration takes a few */
        int v = pal_gpio_read(x->busy);
        if (v == 0) return NEMA_OK;
        if (v < 0) return NEMA_ERR_IO;
        nema_sleep_ms(1);
    }
    return NEMA_ERR_TIMEOUT;
}

static nema_err_t cmd(pal_sx126x_t *x, uint8_t op, const uint8_t *p, int n) {
    uint8_t tx[1 + PAL_LORA_MAX + 1];
    if (n > (int)sizeof(tx) - 1) return NEMA_ERR_INVAL;
    nema_err_t e = wait_busy(x);
    if (e != NEMA_OK) return e;
    tx[0] = op;
    if (n) memcpy(tx + 1, p, (size_t)n);
    return pal_bus_xfer(x->spi, x->cs, tx, n + 1, NULL, 0) == 0 ? NEMA_OK : NEMA_ERR_IO;
}

/* op, then pre (address bytes and the like), then the NOP for the status, then n bytes read */
static nema_err_t rd(pal_sx126x_t *x, uint8_t op, const uint8_t *pre, int npre, uint8_t *out, int n) {
    uint8_t tx[8];
    nema_err_t e = wait_busy(x);
    if (e != NEMA_OK) return e;
    tx[0] = op;
    if (npre) memcpy(tx + 1, pre, (size_t)npre);
    tx[1 + npre] = 0x00;
    return pal_bus_xfer(x->spi, x->cs, tx, npre + 2, out, n) == 0 ? NEMA_OK : NEMA_ERR_IO;
}

#define CMD(x, op, ...)                                                                                                     \
    do {                                                                                                                     \
        const uint8_t a_[] = {__VA_ARGS__};                                                                                  \
        nema_err_t e_ = cmd((x), (op), a_, (int)sizeof(a_));                                                                 \
        if (e_ != NEMA_OK) return e_;                                                                                        \
    } while (0)

static uint16_t irq(pal_sx126x_t *x) {
    uint8_t b[2] = {0, 0};
    if (rd(x, OP_GET_IRQ, NULL, 0, b, 2) != NEMA_OK) return 0;
    return (uint16_t)(b[0] << 8 | b[1]);
}

static uint8_t bw_code(uint16_t khz) {
    switch (khz) {
    case 31: return 0x02;
    case 62: return 0x03;
    case 250: return 0x05;
    case 500: return 0x06;
    default: return 0x04; /* 125 */
    }
}

static nema_err_t start(pal_sx126x_t *x) {
    if (x->reset) {
        pal_gpio_write(x->reset, 0);
        nema_sleep_ms(2);
        pal_gpio_write(x->reset, 1);
        nema_sleep_ms(10);
    }
    CMD(x, OP_SET_STANDBY, 0x00); /* RC oscillator */
    if (x->tcxo_mv) {
        static const uint16_t MV[8] = {1600, 1700, 1800, 2200, 2400, 2700, 3000, 3300};
        uint8_t code = 7;
        for (uint8_t i = 0; i < 8; i++)
            if (MV[i] >= x->tcxo_mv) {
                code = i;
                break;
            }
        CMD(x, OP_DIO3_TCXO, code, 0x00, 0x01, 0x40); /* 5 ms to start: 320 steps of 15.625 us */
        CMD(x, OP_CALIBRATE, 0x7F);                   /* everything again, now on the TCXO */
    }
    CMD(x, OP_SET_REGULATOR, 0x01); /* DC-DC */
    if (x->dio2_rf_switch) CMD(x, OP_DIO2_RF_SWITCH, 0x01);
    CMD(x, OP_SET_PACKET_TYPE, 0x01); /* LoRa */
    CMD(x, OP_SET_BUF_BASE, 0x00, 0x00);
    CMD(x, OP_SET_DIO_IRQ, 0x02, 0x63, 0x02, 0x63, 0x00, 0x00, 0x00, 0x00); /* done, errors and timeout, on DIO1 too */
    x->started = true;
    return NEMA_OK;
}

static nema_err_t configure(void *ctx, const pal_lora_cfg_t *c) {
    pal_sx126x_t *x = (pal_sx126x_t *)ctx;
    if (!x->started) {
        nema_err_t e = start(x);
        if (e != NEMA_OK) return e;
    } else if (!memcmp(&x->cfg, c, sizeof(*c))) {
        return NEMA_OK;
    }
    CMD(x, OP_SET_STANDBY, 0x00);
    uint32_t f = pal_sx126x_freq_reg(c->freq_hz);
    /* the image calibration for the band (datasheet table 9-2) */
    uint32_t mhz = c->freq_hz / 1000000u;
    uint8_t i1 = 0xE1, i2 = 0xE9; /* 902 to 928 */
    if (mhz >= 430 && mhz <= 440) i1 = 0x6B, i2 = 0x6F;
    else if (mhz >= 470 && mhz <= 510) i1 = 0x75, i2 = 0x81;
    else if (mhz >= 779 && mhz <= 787) i1 = 0xC1, i2 = 0xC5;
    else if (mhz >= 863 && mhz <= 870) i1 = 0xD7, i2 = 0xDB;
    CMD(x, OP_CALIBRATE_IMAGE, i1, i2);
    CMD(x, OP_SET_RF_FREQ, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f);
    int8_t p = c->power_dbm;
    if (x->low_power_pa) {
        if (p > 15) p = 15;
        CMD(x, OP_SET_PA_CONFIG, 0x04, 0x00, 0x01, 0x01);
    } else {
        if (p > 22) p = 22;
        CMD(x, OP_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01);
    }
    CMD(x, OP_SET_TX_PARAMS, (uint8_t)p, 0x04); /* 200 us ramp */
    uint32_t tsym_us = (uint32_t)((1000u << c->sf) / c->bw_khz);
    CMD(x, OP_SET_MOD_PARAMS, c->sf, bw_code(c->bw_khz), (uint8_t)(c->cr - 4), tsym_us > 16000u ? 0x01 : 0x00);
    const uint8_t sync[2] = {(uint8_t)((c->sync & 0xF0) | 0x04), (uint8_t)((c->sync & 0x0F) << 4 | 0x04)};
    uint8_t reg[4] = {(uint8_t)(REG_SYNC_WORD >> 8), (uint8_t)REG_SYNC_WORD, sync[0], sync[1]};
    nema_err_t e = cmd(x, OP_WRITE_REG, reg, 4);
    if (e != NEMA_OK) return e;
    x->cfg = *c;
    return NEMA_OK;
}

static nema_err_t packet_params(pal_sx126x_t *x, uint8_t len) {
    CMD(x, OP_SET_PKT_PARAMS, (uint8_t)(x->cfg.preamble >> 8), (uint8_t)x->cfg.preamble, 0x00 /* explicit header */, len,
        0x01 /* CRC */, 0x00 /* IQ normal */);
    return NEMA_OK;
}

static nema_err_t send(void *ctx, const uint8_t *buf, size_t n, int32_t timeout_ms) {
    pal_sx126x_t *x = (pal_sx126x_t *)ctx;
    if (!x->started || n == 0 || n > PAL_LORA_MAX) return NEMA_ERR_STATE;
    nema_err_t e = packet_params(x, (uint8_t)n);
    if (e != NEMA_OK) return e;
    uint8_t wb[1 + PAL_LORA_MAX];
    wb[0] = 0x00; /* buffer offset */
    memcpy(wb + 1, buf, n);
    if ((e = cmd(x, OP_WRITE_BUF, wb, (int)n + 1)) != NEMA_OK) return e;
    CMD(x, OP_CLR_IRQ, 0xFF, 0xFF);
    CMD(x, OP_SET_TX, 0x00, 0x00, 0x00); /* no chip timeout: the loop below has one */
    for (int32_t t = 0; t <= timeout_ms; t += 2) {
        uint16_t s = irq(x);
        if (s & IRQ_TX_DONE) {
            CMD(x, OP_CLR_IRQ, 0xFF, 0xFF);
            return NEMA_OK;
        }
        if (nema_interrupted()) break;
        nema_sleep_ms(2);
    }
    CMD(x, OP_SET_STANDBY, 0x00);
    return nema_interrupted() ? NEMA_ERR_INTR : NEMA_ERR_TIMEOUT;
}

static int recv(void *ctx, uint8_t *buf, size_t cap, int32_t timeout_ms, pal_lora_meta_t *meta) {
    pal_sx126x_t *x = (pal_sx126x_t *)ctx;
    if (!x->started) return NEMA_ERR_STATE;
    nema_err_t e = packet_params(x, 0xFF);
    if (e != NEMA_OK) return e;
    if ((e = cmd(x, OP_CLR_IRQ, (const uint8_t[]){0xFF, 0xFF}, 2)) != NEMA_OK) return e;
    if ((e = cmd(x, OP_SET_RX, (const uint8_t[]){0xFF, 0xFF, 0xFF}, 3)) != NEMA_OK) return e; /* continuous */
    int r = 0;
    for (int32_t t = 0; t <= timeout_ms || timeout_ms < 0; t += 2) {
        uint16_t s = irq(x);
        if (s & IRQ_RX_DONE) {
            if (s & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) {
                r = NEMA_ERR_PROTO;
                break;
            }
            uint8_t st[2] = {0, 0}, ps[3] = {0, 0, 0};
            if (rd(x, OP_GET_RX_BUF, NULL, 0, st, 2) != NEMA_OK) {
                r = NEMA_ERR_IO;
                break;
            }
            size_t len = st[0] < cap ? st[0] : cap;
            if (rd(x, OP_READ_BUF, &st[1], 1, buf, (int)len) != NEMA_OK) {
                r = NEMA_ERR_IO;
                break;
            }
            if (meta && rd(x, OP_GET_PKT_STATUS, NULL, 0, ps, 3) == NEMA_OK) {
                meta->rssi_dbm = (int16_t)(-(int)ps[0] / 2);
                meta->snr_db = (int8_t)((int8_t)ps[1] / 4);
            }
            r = (int)len;
            break;
        }
        if (nema_interrupted()) {
            r = NEMA_ERR_INTR;
            break;
        }
        nema_sleep_ms(2);
    }
    (void)cmd(x, OP_CLR_IRQ, (const uint8_t[]){0xFF, 0xFF}, 2);
    (void)cmd(x, OP_SET_STANDBY, (const uint8_t[]){0x00}, 1);
    return r;
}

const pal_lora_ops_t PAL_SX126X_OPS = {configure, send, recv};
