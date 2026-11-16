/* hhv2 microSD: the card slot on the LCD's SPI bus (SPI2: CLK GPIO14, MOSI GPIO18, MISO GPIO19), chip select GPIO5
 * (see the board pinout documentation). FAT through ESP-IDF's sdspi host, mounted at /sdcard; the system
 * puts it into the namespace as /sd (system/src/sd.c). The SPI driver serialises the card and the panel: a card access
 * waits for the band being sent to the LCD, and the other way round.
 *
 * The slot has no card-detect line, so a card is found by mounting it: at boot, and again on request (`sd mount`, Settings
 * > Storage). A card pulled out while mounted makes file calls fail; `sd eject` before pulling it is the safe way. */
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "hhv2.h"
#include "nema/nema.h"
#include "pal/sd.h"
#include "sdmmc_cmd.h"

#define PIN_SD_CS 5
#define SD_ROOT "/sdcard"
#define SD_KHZ 20000 /* the card's top speed in SPI mode; the bus also runs the panel at 80 MHz on its own CS */

static sdmmc_card_t *s_card;

/* CS high: a card that is not mounted (or not there) stays off the shared bus. The pin was set up as an output at boot
 * (hhv2_hw_init); gpio_config again would report a conflict with the SPI driver that just let it go. */
static void cs_idle(void) {
    gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_SD_CS, 1);
}

static const char *sd_mount(void) {
    if (s_card) return SD_ROOT;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST; /* initialised by hhv2_hw_init for the panel; the card is a second device on it */
    host.max_freq_khz = SD_KHZ;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = SPI2_HOST;
    dev.gpio_cs = PIN_SD_CS;
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16 * 1024};
    esp_err_t e = esp_vfs_fat_sdspi_mount(SD_ROOT, &host, &dev, &mc, &s_card);
    if (e != ESP_OK) {
        s_card = NULL;
        cs_idle();
        NEMA_LOGI("sd", "no card mounted: %s", esp_err_to_name(e));
        return NULL;
    }
    NEMA_LOGI("sd", "card %s, %lu MB, mounted at %s", s_card->cid.name,
              (unsigned long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024)), SD_ROOT);
    return SD_ROOT;
}

static void sd_unmount(void) {
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card); /* removes the card's SPI device; the bus stays for the panel */
    s_card = NULL;
    cs_idle();
}

static bool sd_info(pal_sd_info_t *out) {
    if (!s_card) return false;
    nema_strlcpy(out->name, s_card->cid.name, sizeof(out->name));
    out->size_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    out->speed_khz = (uint32_t)s_card->max_freq_khz;
    return true;
}

static bool sd_space(uint64_t *total, uint64_t *free_bytes) {
    return s_card && esp_vfs_fat_info(SD_ROOT, total, free_bytes) == ESP_OK;
}

static const pal_sd_ops_t SD_OPS = {sd_mount, sd_unmount, sd_info, sd_space};

const pal_sd_ops_t *hhv2_sd_ops(void) { return &SD_OPS; }
