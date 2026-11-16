#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "hhv2_lcd.h"

static const char *TAG = "hhv2_lcd";

#define PIN_SCK     14
#define PIN_MOSI    18
#define PIN_MISO    19
#define PIN_LCD_CS  15
#define PIN_LCD_DC  16
#define PIN_LCD_RST 17
#define PIN_LCD_BL  10
#define PIN_SD_CS   5

#define LCD_HOST   SPI2_HOST
#define LCD_CLK_HZ (40 * 1000 * 1000)
#define BUF_PIXELS (HHV2_LCD_W * 40)

#define SW(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))

static esp_lcd_panel_handle_t panel;
static uint16_t *buf;
static SemaphoreHandle_t trans_done;
static SemaphoreHandle_t lock;

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(trans_done, &woken);
    return woken == pdTRUE;
}

// Send buf to the area (x,y,w,h), then wait for the DMA to finish
static void blit(int x, int y, int w, int h)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, buf));
    xSemaphoreTake(trans_done, portMAX_DELAY);
}

void hhv2_lcd_init(void)
{
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
        .max_transfer_sz = BUF_PIXELS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    trans_done = xSemaphoreCreateBinary();
    lock = xSemaphoreCreateMutex();

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
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    buf = heap_caps_malloc(BUF_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(buf);
    ESP_LOGI(TAG, "ILI9341 %dx%d ready", HHV2_LCD_W, HHV2_LCD_H);
}

void hhv2_lcd_backlight(bool on)
{
    gpio_set_level(PIN_LCD_BL, on ? 1 : 0);
}

void hhv2_lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    uint16_t c = SW(color);
    int rows_per = BUF_PIXELS / w;
    if (rows_per > h) rows_per = h;
    for (int i = 0; i < w * rows_per; i++) buf[i] = c;
    for (int yy = y; yy < y + h; yy += rows_per) {
        int hh = (y + h - yy) < rows_per ? (y + h - yy) : rows_per;
        blit(x, yy, w, hh);
    }
    xSemaphoreGive(lock);
}

int hhv2_lcd_text_w(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

void hhv2_lcd_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    int w = hhv2_lcd_text_w(s, scale);
    int h = 7 * scale;
    if (w <= 0) return;
    if (x + w > HHV2_LCD_W) w = HHV2_LCD_W - x;
    if (w <= 0 || w * h > BUF_PIXELS) return;

    xSemaphoreTake(lock, portMAX_DELAY);
    uint16_t f = SW(fg), b = SW(bg);
    for (int i = 0; i < w * h; i++) buf[i] = b;
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = hhv2_font_glyph(s[i]);
        int cx = i * 6 * scale;
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px = cx + c * scale + dx;
                        if (px < w) buf[(r * scale + dy) * w + px] = f;
                    }
                }
            }
        }
    }
    blit(x, y, w, h);
    xSemaphoreGive(lock);
}
