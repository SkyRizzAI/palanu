// Bring-up 01: ILI9341 LCD on the hhv2 board (ESP32-S31-WROOM-3)
// Pins from the board pinout documentation
//   SPI shared LCD+SD: SCK=GPIO14 MOSI=GPIO18 MISO=GPIO19
//   LCD: CS=GPIO15 DC=GPIO16 RST=GPIO17, backlight GPIO10 (HIGH = on, via Q4)
//   SD CS=GPIO5 (pull-up 10K), held HIGH so the SD card stays quiet on the bus

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"

static const char *TAG = "lcd";

#define PIN_SCK   14
#define PIN_MOSI  18
#define PIN_MISO  19
#define PIN_LCD_CS 15
#define PIN_LCD_DC 16
#define PIN_LCD_RST 17
#define PIN_LCD_BL 10
#define PIN_SD_CS  5

#define LCD_W 320
#define LCD_H 240
#define LCD_HOST SPI2_HOST
#define LCD_CLK_HZ (40 * 1000 * 1000)
#define BAND_H 40  // height of one transfer slice

// RGB565, byte-swapped for SPI (sent big-endian)
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define SW(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))

static esp_lcd_panel_handle_t panel;
static uint16_t *band;
static SemaphoreHandle_t trans_done;

// Called from the ISR when a colour transfer (DMA) finishes, so the band buffer
// is not overwritten before the old data has been sent to the LCD.
static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(trans_done, &woken);
    return woken == pdTRUE;
}

// ---- minimal 5x7 font (only the characters in use) ----
typedef struct { char ch; uint8_t rows[7]; } glyph_t;
static const glyph_t FONT[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'-', {0, 0, 0, 0x1F, 0, 0, 0}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
};

static const uint8_t *glyph(char c)
{
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].ch == c) return FONT[i].rows;
    }
    return FONT[0].rows;
}

// Draw text into the band buffer (y is relative to the band), scale s
static void band_text(int y0_band, int x0, int y0, const char *s, int scale, uint16_t color)
{
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = glyph(s[i]);
        int cx = x0 + i * 6 * scale;
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int dy = 0; dy < scale; dy++) {
                    int y = y0 + r * scale + dy - y0_band;
                    if (y < 0 || y >= BAND_H) continue;
                    for (int dx = 0; dx < scale; dx++) {
                        int x = cx + c * scale + dx;
                        if (x >= 0 && x < LCD_W) band[y * LCD_W + x] = SW(color);
                    }
                }
            }
        }
    }
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale - scale; }

static void draw_screen(void)
{
    const char *title = "HELLO PALANU";
    const char *sub = "ESP32-S31 HHV2";
    const int t_scale = 4, s_scale = 2;
    const int t_y = 70, s_y = 120;
    const uint16_t bars[] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 255)};
    const char *bar_lbl[] = {"R", "G", "B", "W"};
    const int bar_y = 180;  // colour bars from y=180 to the bottom (60 px)

    for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
        for (int y = 0; y < BAND_H; y++) {
            int gy = y0 + y;
            for (int x = 0; x < LCD_W; x++) {
                uint16_t c;
                if (gy >= bar_y) c = bars[x / (LCD_W / 4)];
                else c = RGB(10, 20, 60 + gy / 2);  // dark blue gradient
                band[y * LCD_W + x] = SW(c);
            }
        }
        band_text(y0, (LCD_W - text_w(title, t_scale)) / 2, t_y, title, t_scale, RGB(255, 210, 0));
        band_text(y0, (LCD_W - text_w(sub, s_scale)) / 2, s_y, sub, s_scale, RGB(220, 220, 220));
        for (int i = 0; i < 4; i++) {
            band_text(y0, i * (LCD_W / 4) + 34, bar_y + 23, bar_lbl[i], 2, RGB(0, 0, 0));
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y0, LCD_W, y0 + BAND_H, band));
        xSemaphoreTake(trans_done, portMAX_DELAY);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Bring-up ILI9341 LCD hhv2");

    // SD CS stays HIGH so the card does not talk on the SPI bus
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_SD_CS) | (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_level(PIN_LCD_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * BAND_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_trans_done,
    };
    trans_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // Landscape 320x240. Confirmed from a photo: without mirroring the image is rotated 180 degrees,
    // and the hhv2 panel shows inverted colours unless it is inverted.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    band = heap_caps_malloc(LCD_W * BAND_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(band);

    draw_screen();
    gpio_set_level(PIN_LCD_BL, 1);
    ESP_LOGI(TAG, "Screen drawn, backlight ON (GPIO%d)", PIN_LCD_BL);

    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
