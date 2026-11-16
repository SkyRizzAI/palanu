// Bring-up 04: launcher UI test (LVGL 9) on the hhv2 board
//   main.c : LCD ILI9341 (SPI 80 MHz), backlight PWM, buzzer, LVGL port
//   kb.c   : TCA8418 keyboard as the LVGL keypad input
//   ui.c   : launcher and sample apps

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "kb.h"
#include "ui.h"

static const char *TAG = "main";

#define PIN_SCK     14
#define PIN_MOSI    18
#define PIN_MISO    19
#define PIN_LCD_CS  15
#define PIN_LCD_DC  16
#define PIN_LCD_RST 17
#define PIN_LCD_BL  10
#define PIN_SD_CS   5
#define PIN_BUZZER  52

#define LCD_W 320
#define LCD_H 240
#define LCD_CLK_HZ (80 * 1000 * 1000)   // proven stable in 03_gfx
#define DRAW_LINES 40                    // 320*40*2 = 25 KB, under the 32 KB limit per SPI transaction

// ---------------------------------------------------------------- backlight + buzzer
#define BL_CH  LEDC_CHANNEL_0
#define BZ_CH  LEDC_CHANNEL_1

static esp_timer_handle_t click_timer;
static bool click_on = true;

static void pwm_init(void)
{
    ledc_timer_config_t bl_t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_t));
    ledc_channel_config_t bl_c = {
        .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = BL_CH,
        .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_c));

    ledc_timer_config_t bz_t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1, .freq_hz = 3000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bz_t));
    ledc_channel_config_t bz_c = {
        .gpio_num = PIN_BUZZER, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = BZ_CH,
        .timer_sel = LEDC_TIMER_1, .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bz_c));
}

void board_set_brightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CH, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CH);
}

static void click_off(void *arg)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BZ_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BZ_CH);
}

void board_click(void)
{
    if (!click_on) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BZ_CH, 300);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BZ_CH);
    esp_timer_stop(click_timer);
    esp_timer_start_once(click_timer, 6000);
}

void board_set_click_enabled(bool en) { click_on = en; }
bool board_click_enabled(void) { return click_on; }

// ---------------------------------------------------------------- LCD
static void lcd_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out)
{
    gpio_config_t g = {.pin_bit_mask = 1ULL << PIN_SD_CS, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&g));
    gpio_set_level(PIN_SD_CS, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK, .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * DRAW_LINES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t ioc = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0, .pclk_hz = LCD_CLK_HZ, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, io_out));

    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(*io_out, &pc, panel_out));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*panel_out));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*panel_out));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(*panel_out, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*panel_out, true));
}

void app_main(void)
{
    pwm_init();
    esp_timer_create_args_t ta = {.callback = click_off, .name = "click"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &click_timer));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    lcd_init(&io, &panel);

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;      // LVGL on core 1
    port_cfg.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_W * DRAW_LINES,
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .rotation = {.swap_xy = true, .mirror_x = true, .mirror_y = true},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    assert(disp);

    lvgl_port_lock(0);
    lv_indev_t *kb = kb_init();
    ui_init(disp, kb);
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(60));  // wait for the first frame before the backlight turns on
    board_set_brightness(80);
    ESP_LOGI(TAG, "Launcher ready");
}
