#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_littlefs.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hhv2.h"
#include "led_strip.h"
#include "nema/nema.h"

/* ---- pins (see the board pinout documentation) ---- */
#define PIN_SCK 14
#define PIN_MOSI 18
#define PIN_MISO 19
#define PIN_LCD_CS 15
#define PIN_LCD_DC 16
#define PIN_LCD_RST 17
#define PIN_LCD_BL 10
#define PIN_SD_CS 5 /* shares the SPI bus: kept high so the card stays silent */
#define PIN_SCL 12
#define PIN_SDA 13
#define PIN_PBRST 6
#define PIN_PBINT 7
#define PIN_RGB 51
#define PIN_BUZZER 52

#define LCD_W HHV2_LCD_W
#define LCD_H HHV2_LCD_H
#define BAND_ROWS 40                        /* 320 * 40 * 2 = 25.6 KB: under the 32 KB limit of one SPI transaction */
#define BAND_BYTES (LCD_W * BAND_ROWS * 2)
#define LCD_CLK_HZ (80 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_trans_done;
static uint16_t *s_band;
static led_strip_handle_t s_led;
static esp_timer_handle_t s_beep_timer;
static nema_owner_t s_owner;

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

/* ---------------------------------------------------------------- display */
static void lcd_present(void *ctx, const uint8_t *px, int stride, pal_rect_t r, const uint16_t *pal) {
    int rows_per = BAND_BYTES / (r.w * 2);
    if (rows_per > BAND_ROWS) rows_per = BAND_ROWS;
    for (int y0 = r.y; y0 < r.y + r.h; y0 += rows_per) {
        int rows = r.y + r.h - y0 < rows_per ? r.y + r.h - y0 : rows_per;
        uint16_t *d = s_band;
        for (int y = 0; y < rows; y++) {
            const uint8_t *src = px + (y0 + y) * stride + r.x;
            for (int x = 0; x < r.w; x++) *d++ = pal[src[x]];
        }
        esp_lcd_panel_draw_bitmap(s_panel, r.x, y0, r.x + r.w, y0 + rows, s_band);
        xSemaphoreTake(s_trans_done, portMAX_DELAY);
    }
}

/* Full colour (Athena): the same bands, the pixels only byte-swapped into the panel's bus order. */
static void lcd_present565(void *ctx, const uint16_t *px, int stride, pal_rect_t r) {
    int rows_per = BAND_BYTES / (r.w * 2);
    if (rows_per > BAND_ROWS) rows_per = BAND_ROWS;
    for (int y0 = r.y; y0 < r.y + r.h; y0 += rows_per) {
        int rows = r.y + r.h - y0 < rows_per ? r.y + r.h - y0 : rows_per;
        uint16_t *d = s_band;
        for (int y = 0; y < rows; y++) {
            const uint16_t *src = px + (y0 + y) * stride + r.x;
            for (int x = 0; x < r.w; x++) *d++ = (uint16_t)((src[x] >> 8) | (src[x] << 8));
        }
        esp_lcd_panel_draw_bitmap(s_panel, r.x, y0, r.x + r.w, y0 + rows, s_band);
        xSemaphoreTake(s_trans_done, portMAX_DELAY);
    }
}

static void backlight(void *ctx, int pct) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)(pct * 1023 / 100));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

pal_display_ops_t hhv2_display_ops(void) {
    pal_display_ops_t o = {.width = LCD_W, .height = LCD_H, .present = lcd_present, .backlight = backlight,
                           .present_rgb565 = lcd_present565};
    return o;
}

/* ---------------------------------------------------------------- buzzer, LED, reboot */
static void beep_off(void *arg) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void hhv2_beep(int freq, int ms) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, (uint32_t)freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    esp_timer_stop(s_beep_timer);
    esp_timer_start_once(s_beep_timer, (uint64_t)ms * 1000u);
}

void hhv2_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

void hhv2_reboot(void) { esp_restart(); }

/* ---------------------------------------------------------------- battery
 * GPIO42 is ADC1 channel 0 behind a 47k/47k divider, so the supply is twice what the pin sees.
 * The S31 ADC is 17 bits with a single attenuation and this IDF has no calibration scheme for it yet, so the
 * conversion below assumes BATTERY_ADC_FULL_SCALE_MV. That constant is NOT verified against a meter: with a
 * battery attached, compare `battery` with a multimeter and correct it. With no battery the pin reads 0. */
#define BATTERY_ADC_FULL_SCALE_MV 3100
#define BATTERY_ADC_MAX 131071

static adc_oneshot_unit_handle_t s_adc;

static void battery_init(void) {
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
    esp_err_t e = adc_oneshot_new_unit(&u, &s_adc);
    if (e != ESP_OK) {
        NEMA_LOGW("board", "battery ADC unit: %s", esp_err_to_name(e));
        s_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_0, .bitwidth = ADC_BITWIDTH_DEFAULT};
    e = adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &c);
    if (e != ESP_OK) {
        NEMA_LOGW("board", "battery ADC channel: %s", esp_err_to_name(e));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
}

static int battery_raw(void) {
    if (!s_adc) return -1;
    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) == ESP_OK) sum += raw, n++;
    }
    return n ? sum / n : -1;
}

/* Below about 2.0 V at the supply the divider is open or no cell is attached: report "unknown", not 0 V. */
int hhv2_battery_mv(void) {
    int raw = battery_raw();
    if (raw < 0) return -1;
    int mv = (int)((int64_t)raw * BATTERY_ADC_FULL_SCALE_MV / BATTERY_ADC_MAX) * 2;
    return mv < 2000 ? -1 : mv;
}

/* A single-cell lithium discharge curve; above the full voltage the board is on USB power. */
int hhv2_battery_percent(void) {
    static const struct { int mv, pct; } curve[] = {{3300, 0}, {3500, 5}, {3600, 15}, {3700, 35}, {3800, 55},
                                                    {3900, 70}, {4000, 82}, {4100, 92}, {4200, 100}};
    int mv = hhv2_battery_mv();
    if (mv < 0) return -1;
    if (mv <= curve[0].mv) return 0;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++)
        if (mv <= curve[i].mv)
            return curve[i - 1].pct + (curve[i].pct - curve[i - 1].pct) * (mv - curve[i - 1].mv) / (curve[i].mv - curve[i - 1].mv);
    return 100;
}

const char *hhv2_storage_init(void) {
    esp_vfs_littlefs_conf_t conf = {.base_path = "/lfs", .partition_label = "storage", .format_if_mount_failed = true};
    return esp_vfs_littlefs_register(&conf) == ESP_OK ? "/lfs" : NULL;
}

void hhv2_hw_init(void) {
    s_owner = nema_owner_register("board");
    battery_init();
    /* the card's CS high until hhv2_sd.c mounts it (the PCB also pulls it up). Not gpio_config: that reserves the pin,
     * and the SD driver taking it later would log a conflict */
    gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_SD_CS, 1);

    ledc_timer_config_t bl_t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&bl_t);
    ledc_channel_config_t bl_c = {.gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                  .timer_sel = LEDC_TIMER_0, .duty = 0};
    ledc_channel_config(&bl_c);
    ledc_timer_config_t bz_t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                                .timer_num = LEDC_TIMER_1, .freq_hz = 2700, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&bz_t);
    ledc_channel_config_t bz_c = {.gpio_num = PIN_BUZZER, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
                                  .timer_sel = LEDC_TIMER_1, .duty = 0};
    ledc_channel_config(&bz_c);
    esp_timer_create_args_t ta = {.callback = beep_off, .name = "beep"};
    esp_timer_create(&ta, &s_beep_timer);

    led_strip_config_t sc = {.strip_gpio_num = PIN_RGB, .max_leds = 1, .led_model = LED_MODEL_WS2812,
                             .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB};
    led_strip_rmt_config_t rc = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&sc, &rc, &s_led) == ESP_OK) led_strip_clear(s_led);

    spi_bus_config_t bus = {.sclk_io_num = PIN_SCK, .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = BAND_BYTES};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    s_trans_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = {.cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC, .spi_mode = 0,
                                         .pclk_hz = LCD_CLK_HZ, .trans_queue_depth = 4, .lcd_cmd_bits = 8,
                                         .lcd_param_bits = 8, .on_color_trans_done = on_trans_done};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io));
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
                                     .bits_per_pixel = 16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &pc, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* verified on the board in tests/hw/01_lcd: landscape needs swap + mirror, and the panel wants inversion */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    s_band = nema_alloc_ex(NEMA_MEM_DMA, s_owner, BAND_BYTES, 0, NEMA_ALLOC_CRITICAL);
    NEMA_CHECK(s_band != NULL);
}

/* ---------------------------------------------------------------- keyboard (TCA8418) */
#define TCA_ADDR 0x34
#define REG_CFG 0x01
#define REG_INT_STAT 0x02
#define REG_KEY_LCK_EC 0x03
#define REG_KEY_EVENT 0x04
#define REG_KP_GPIO1 0x1D
#define REG_KP_GPIO2 0x1E
#define REG_KP_GPIO3 0x1F

static i2c_master_dev_handle_t s_tca;

static esp_err_t tca_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_tca, b, 2, 50);
}
static esp_err_t tca_read(uint8_t reg, uint8_t *val) { return i2c_master_transmit_receive(s_tca, &reg, 1, val, 1, 50); }

static void keyboard_task(void *arg) {
    for (;;) {
        if (gpio_get_level(PIN_PBINT) == 0) {
            uint8_t ec = 0;
            if (tca_read(REG_KEY_LCK_EC, &ec) == ESP_OK) {
                for (int n = ec & 0x0F; n > 0; n--) {
                    uint8_t ev;
                    if (tca_read(REG_KEY_EVENT, &ev) != ESP_OK || ev == 0) break;
                    int code = ev & 0x7F;
                    if (code >= 1 && code <= 40) pal_input_hw((code - 1) / 10, (code - 1) % 10, (ev & 0x80) != 0);
                }
                tca_write(REG_INT_STAT, 0x01);
            }
        }
        nema_sleep_ms(8);
    }
}

void hhv2_keyboard_start(void) {
    gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_PBRST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_set_level(PIN_PBRST, 0);
    nema_port_sleep_ms(2);
    gpio_set_level(PIN_PBRST, 1);
    nema_port_sleep_ms(5);
    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_PBINT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&in);
    i2c_master_bus_config_t bc = {.i2c_port = -1, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
                                  .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7};
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA_ADDR, .scl_speed_hz = 400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &s_tca));
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO1, 0x0F)); /* rows R0-R3 */
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO2, 0xFF)); /* columns C0-C7 */
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO3, 0x03)); /* columns C8-C9 */
    uint8_t ev;
    for (int i = 0; i < 16; i++) tca_read(REG_KEY_EVENT, &ev);
    ESP_ERROR_CHECK(tca_write(REG_INT_STAT, 0x1F));
    ESP_ERROR_CHECK(tca_write(REG_CFG, 0x01));
    nema_task_desc_t td = {.name = "keys", .fn = keyboard_task, .stack = 3072, .prio = 7, .core = -1,
                           .role = NEMA_TASK_SYSTEM, .owner = s_owner};
    ESP_ERROR_CHECK(nema_task_create(&td));
}
