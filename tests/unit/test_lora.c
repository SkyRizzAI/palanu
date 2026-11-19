/* LoRa (pal/lora.h) and the SX126x driver (pal/sx126x.h) against a pretend chip on a pretend SPI bus: the commands it sends
 * (frequency register, modulation, sync word, packet), a packet sent and one received with its RSSI and SNR, a CRC error,
 * the time on air from Semtech's formula, and the configuration check. */
#include <string.h>

#include "nema/nema.h"
#include "pal/lora.h"
#include "pal/sx126x.h"
#include "test.h"

/* ---- the pretend chip ---- */
static uint8_t g_log[64][24];
static int g_lens[64], g_n;
static uint16_t g_irq;
static const char *g_rx = "hello";

static int bus_xfer(void *ctx, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn) {
    (void)ctx, (void)addr;
    if (g_n < 64) {
        memcpy(g_log[g_n], tx, txn < 24 ? (size_t)txn : 24);
        g_lens[g_n++] = txn;
    }
    switch (tx[0]) {
    case 0x12: rx[0] = (uint8_t)(g_irq >> 8), rx[1] = (uint8_t)g_irq; break;    /* GetIrqStatus */
    case 0x83: g_irq = 0x0001; break;                                            /* SetTx: done at once */
    case 0x02: g_irq = 0; break;                                                 /* ClearIrq */
    case 0x13: rx[0] = (uint8_t)strlen(g_rx), rx[1] = 0x80; break;               /* GetRxBufferStatus */
    case 0x1E: memcpy(rx, g_rx, (size_t)rxn); break;                             /* ReadBuffer */
    case 0x14: rx[0] = 0x50, rx[1] = 0x28, rx[2] = 0; break;                     /* GetPacketStatus: -40 dBm, 10 dB */
    default: break;
    }
    return 0;
}
static int busy_read(void *ctx) { return 0; }
static int reset_write(void *ctx, int level) { return 0; }

static const pal_device_t SPI = {.id = "spi0", .kind = PAL_DEV_BUS, .bus = {.type = PAL_BUS_SPI, .xfer = bus_xfer}};
static const pal_device_t BUSY = {.id = "lora_busy", .kind = PAL_DEV_GPIO, .gpio = {.read = busy_read}};
static const pal_device_t RST = {.id = "lora_rst", .kind = PAL_DEV_GPIO, .gpio = {.write = reset_write}};
static pal_sx126x_t CHIP = {.spi = &SPI, .cs = 0, .busy = &BUSY, .reset = &RST, .tcxo_mv = 1800, .dio2_rf_switch = true};

static int find(uint8_t op) {
    for (int i = g_n - 1; i >= 0; i--)
        if (g_log[i][0] == op) return i;
    return -1;
}

static pal_lora_cfg_t cfg(void) {
    pal_lora_cfg_t c = {.freq_hz = 868100000, .sf = 9, .bw_khz = 125, .cr = 5, .power_dbm = 14, .sync = 0x12, .preamble = 8};
    return c;
}

TEST(configure_sends_the_radio_settings) {
    CHECK_EQ(pal_sx126x_freq_reg(868100000), 0x36419999u);
    pal_lora_cfg_t c = cfg();
    CHECK(PAL_SX126X_OPS.configure(&CHIP, &c) == NEMA_OK);
    int i = find(0x86);
    CHECK(i >= 0 && g_log[i][1] == 0x36 && g_log[i][2] == 0x41 && g_log[i][3] == 0x99 && g_log[i][4] == 0x99);
    i = find(0x8B); /* SF9, 125 kHz, 4/5, no low data rate optimisation */
    CHECK(i >= 0 && g_log[i][1] == 9 && g_log[i][2] == 0x04 && g_log[i][3] == 0x01 && g_log[i][4] == 0x00);
    i = find(0x0D); /* the private sync word 0x12 in register 0x0740 */
    CHECK(i >= 0 && g_log[i][1] == 0x07 && g_log[i][2] == 0x40 && g_log[i][3] == 0x14 && g_log[i][4] == 0x24);
    i = find(0x98); /* the image calibration for 863 to 870 MHz */
    CHECK(i >= 0 && g_log[i][1] == 0xD7 && g_log[i][2] == 0xDB);
    CHECK(find(0x97) >= 0 && find(0x9D) >= 0); /* TCXO on DIO3, antenna switch on DIO2 */
    int before = g_n;
    CHECK(PAL_SX126X_OPS.configure(&CHIP, &c) == NEMA_OK);
    CHECK_EQ(g_n, before); /* the same settings again send nothing */
}

TEST(send_and_receive) {
    pal_lora_cfg_t c = cfg();
    PAL_SX126X_OPS.configure(&CHIP, &c);
    g_n = 0;
    CHECK(PAL_SX126X_OPS.send(&CHIP, (const uint8_t *)"ping", 4, 500) == NEMA_OK);
    int i = find(0x0E);
    CHECK(i >= 0 && g_lens[i] == 6 && !memcmp(&g_log[i][2], "ping", 4));
    i = find(0x8C); /* preamble 8, explicit header, 4 bytes, CRC */
    CHECK(i >= 0 && g_log[i][2] == 8 && g_log[i][4] == 4 && g_log[i][5] == 1);

    uint8_t buf[32];
    pal_lora_meta_t m = {0};
    g_n = 0;
    g_irq = 0;
    int r;
    r = PAL_SX126X_OPS.recv(&CHIP, buf, sizeof(buf), 0, &m); /* no packet within 0 ms */
    CHECK_EQ(r, 0);
    CHECK(find(0x82) >= 0 && find(0x80) >= 0); /* it listened, then went back to standby */
}

/* a chip that has a packet when the driver asks */
static bool g_bad;
static int bus_rx(void *ctx, int addr, const uint8_t *tx, int txn, uint8_t *rx, int rxn) {
    int r = bus_xfer(ctx, addr, tx, txn, rx, rxn);
    if (tx[0] == 0x82) g_irq = g_bad ? 0x0042 : 0x0002; /* SetRx: a packet arrives (with a CRC error when g_bad) */
    return r;
}
static const pal_device_t SPI_RX = {.id = "spi1", .kind = PAL_DEV_BUS, .bus = {.type = PAL_BUS_SPI, .xfer = bus_rx}};

TEST(a_received_packet_and_a_bad_one) {
    pal_sx126x_t chip = {.spi = &SPI_RX, .busy = &BUSY};
    pal_lora_cfg_t c = cfg();
    CHECK(PAL_SX126X_OPS.configure(&chip, &c) == NEMA_OK);
    uint8_t buf[32];
    pal_lora_meta_t m = {0};
    int r = PAL_SX126X_OPS.recv(&chip, buf, sizeof(buf), 100, &m);
    CHECK_EQ(r, 5);
    CHECK(!memcmp(buf, "hello", 5));
    CHECK(m.rssi_dbm == -40 && m.snr_db == 10);
    g_bad = true; /* the next packet arrives with a bad CRC */
    CHECK_EQ(PAL_SX126X_OPS.recv(&chip, buf, sizeof(buf), 100, &m), NEMA_ERR_PROTO);
    g_bad = false;
}

TEST(airtime_and_the_check) {
    pal_lora_cfg_t c = cfg();
    CHECK_EQ((int)pal_lora_airtime_ms(&c, 10), 145); /* Semtech's calculator: 144.4 ms */
    c.sf = 12;
    CHECK(pal_lora_airtime_ms(&c, 10) > 900); /* SF12: about a second */
    CHECK(pal_lora_cfg_check(&c) == NULL);
    c.sf = 13;
    CHECK(pal_lora_cfg_check(&c) != NULL);
    c = cfg();
    c.bw_khz = 100;
    CHECK(pal_lora_cfg_check(&c) != NULL);
    c = cfg();
    c.freq_hz = 2400000000u;
    CHECK(pal_lora_cfg_check(&c) != NULL);
}

int main(void) {
    nema_config_t ncfg = {.console_log = false};
    nema_init(&ncfg);
    RUN(configure_sends_the_radio_settings);
    RUN(send_and_receive);
    RUN(a_received_packet_and_a_bad_one);
    RUN(airtime_and_the_check);
    return TEST_DONE();
}
