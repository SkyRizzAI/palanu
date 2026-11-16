// Bring-up 02: 4x10 keyboard through the TCA8418 on the hhv2 board
//   I2C: SCL=GPIO12 SDA=GPIO13 (external 2K2 pull-ups), TCA8418 @ 0x34
//   PBRST=GPIO6 (TCA8418 reset, active LOW), PBINT=GPIO7 (interrupt, active LOW)
//   Matrix: rows R0-R3, columns C0-C9 (39 keys fitted)
//   Feedback: WS2812 LED D3 on GPIO51, buzzer on GPIO52 (LEDC), BOOT button GPIO61
//
// The screen shows a 4x10 grid of matrix positions. Every event is also logged on
// serial, so the position -> key label map can be built from the press order.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "led_strip.h"
#include "hhv2_lcd.h"
#include "hhv2_keymap.h"

static const char *TAG = "kbd";

#define PIN_SCL    12
#define PIN_SDA    13
#define PIN_PBRST  6
#define PIN_PBINT  7
#define PIN_RGB    51
#define PIN_BUZZER 52
#define PIN_BOOT   61

#define TCA_ADDR       0x34
#define REG_CFG        0x01
#define REG_INT_STAT   0x02
#define REG_KEY_LCK_EC 0x03
#define REG_KEY_EVENT  0x04
#define REG_KP_GPIO1   0x1D
#define REG_KP_GPIO2   0x1E
#define REG_KP_GPIO3   0x1F

#define ROWS 4
#define COLS 10

// ---- grid display ----
#define CELL   30
#define GAP    2
#define GRID_X 1
#define GRID_Y 26

#define C_BG     HHV2_RGB(10, 14, 30)
#define C_NEVER  HHV2_RGB(45, 45, 60)
#define C_SEEN   HHV2_RGB(0, 110, 60)
#define C_DOWN   HHV2_RGB(255, 200, 0)
#define C_TEXT   HHV2_RGB(230, 230, 230)
#define C_TITLE  HHV2_RGB(255, 200, 0)

static i2c_master_dev_handle_t tca;
static led_strip_handle_t rgb;
static bool seen[ROWS][COLS];
static bool down[ROWS][COLS];
static int seen_count;
static uint32_t event_count;

// ---------- TCA8418 ----------
static esp_err_t tca_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(tca, b, 2, 100);
}

static esp_err_t tca_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(tca, &reg, 1, val, 1, 100);
}

static bool tca_init(void)
{
    gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_PBRST, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_PBRST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_PBRST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_PBINT) | (1ULL << PIN_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    if (i2c_master_probe(bus, TCA_ADDR, 100) != ESP_OK) {
        ESP_LOGE(TAG, "TCA8418 does not answer at 0x%02X", TCA_ADDR);
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &tca));

    // R0-R3 and C0-C9 as the keypad matrix
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO1, 0x0F));
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO2, 0xFF));
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO3, 0x03));
    // Drain the leftover FIFO, then enable the key event interrupt (KE_IEN)
    uint8_t ev;
    for (int i = 0; i < 16; i++) tca_read(REG_KEY_EVENT, &ev);
    ESP_ERROR_CHECK(tca_write(REG_INT_STAT, 0x1F));
    ESP_ERROR_CHECK(tca_write(REG_CFG, 0x01));
    ESP_LOGI(TAG, "TCA8418 ready (4x10, KE_IEN)");
    return true;
}

// ---------- LED and buzzer ----------
static void feedback_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_RGB,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &rgb));
    led_strip_clear(rgb);

    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 2700,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));
    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

static void beep(int ms)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Dim rainbow colour per key position
static void led_for_key(int idx)
{
    int h = (idx * 360 / (ROWS * COLS)) % 360;
    int x = (h % 60) * 255 / 60;
    int r, g, b;
    switch (h / 60) {
    case 0: r = 255; g = x; b = 0; break;
    case 1: r = 255 - x; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = x; break;
    case 3: r = 0; g = 255 - x; b = 255; break;
    case 4: r = x; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - x; break;
    }
    led_strip_set_pixel(rgb, 0, r / 6, g / 6, b / 6);
    led_strip_refresh(rgb);
}

// ---------- display ----------
// The grid is drawn in physical order: the matrix columns are reversed (C9 on the left)
static void draw_cell(int r, int c)
{
    int x = GRID_X + (COLS - 1 - c) * (CELL + GAP);
    int y = GRID_Y + r * (CELL + GAP);
    uint16_t key = HHV2_KEYMAP[r][c];
    if (key == HHV2_KEY_NONE) {
        hhv2_lcd_fill(x, y, CELL, CELL, C_BG);
        return;
    }
    uint16_t bg = down[r][c] ? C_DOWN : (seen[r][c] ? C_SEEN : C_NEVER);
    uint16_t fg = down[r][c] ? HHV2_RGB(0, 0, 0) : C_TEXT;
    hhv2_lcd_fill(x, y, CELL, CELL, bg);
    const char *lbl = hhv2_key_label(key);
    int scale = strlen(lbl) == 1 ? 2 : 1;
    hhv2_lcd_text(x + (CELL - hhv2_lcd_text_w(lbl, scale)) / 2, y + (CELL - 7 * scale) / 2,
                  lbl, scale, fg, bg);
}

static void draw_status(const char *last)
{
    char line[40];
    hhv2_lcd_fill(0, 162, HHV2_LCD_W, 40, C_BG);
    hhv2_lcd_text(4, 164, last, 2, C_TEXT, C_BG);
    snprintf(line, sizeof(line), "DETECTED: %d/39", seen_count);
    hhv2_lcd_text(4, 186, line, 2, seen_count >= 39 ? C_TITLE : C_TEXT, C_BG);
}

static void draw_all(void)
{
    hhv2_lcd_fill(0, 0, HHV2_LCD_W, HHV2_LCD_H, C_BG);
    hhv2_lcd_text(4, 4, "KEYBOARD TEST TCA8418", 2, C_TITLE, C_BG);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) draw_cell(r, c);
    draw_status("PRESS A KEY...");
    hhv2_lcd_text(4, 214, "PRESS ALL 39 KEYS ONE BY ONE", 1, C_TEXT, C_BG);
    hhv2_lcd_text(4, 226, "BOOT = START OVER", 1, C_TEXT, C_BG);
}

// ---------- events ----------
static void handle_event(uint8_t ev)
{
    bool pressed = ev & 0x80;
    int code = ev & 0x7F;
    if (code < 1 || code > 80) {
        ESP_LOGW(TAG, "non-keypad event 0x%02X", ev);
        return;
    }
    int r = (code - 1) / 10;
    int c = (code - 1) % 10;
    event_count++;
    if (r >= ROWS || c >= COLS) {
        ESP_LOGW(TAG, "position outside the 4x10 matrix (code %d)", code);
        return;
    }
    const char *name = hhv2_key_label(HHV2_KEYMAP[r][c]);
    ESP_LOGI(TAG, "#%lu KEY %s r=%d c=%d (code %d) %s", (unsigned long)event_count,
             pressed ? "DOWN" : "UP  ", r, c, code, name);
    down[r][c] = pressed;
    if (pressed && !seen[r][c]) {
        seen[r][c] = true;
        seen_count++;
    }
    draw_cell(r, c);
    if (pressed) {
        char last[32];
        snprintf(last, sizeof(last), "KEY: %s (R%d C%d)", name, r, c);
        draw_status(last);
        led_for_key(r * COLS + c);
        beep(25);
        if (seen_count == 39) ESP_LOGI(TAG, "All 39 keys detected");
    } else {
        led_strip_clear(rgb);
    }
}

static void reset_progress(void)
{
    memset(seen, 0, sizeof(seen));
    memset(down, 0, sizeof(down));
    seen_count = 0;
    ESP_LOGI(TAG, "Count reset (BOOT)");
    draw_all();
    beep(80);
}

void app_main(void)
{
    hhv2_lcd_init();
    hhv2_lcd_fill(0, 0, HHV2_LCD_W, HHV2_LCD_H, C_BG);
    hhv2_lcd_backlight(true);
    feedback_init();

    if (!tca_init()) {
        hhv2_lcd_text(4, 100, "TCA8418 NOT DETECTED", 2, HHV2_RGB(255, 60, 60), C_BG);
        return;
    }
    draw_all();
    beep(40);

    bool boot_prev = true;
    bool pbint_seen = false;
    int poll_div = 0;
    while (true) {
        // PBINT is active LOW while there are events in the FIFO. As a fallback, the FIFO
        // is also read every ~50 ms even if PBINT does not go low, so that a problem on the
        // interrupt line can be told apart from a problem in the matrix.
        bool pbint = gpio_get_level(PIN_PBINT) == 0;
        if (pbint || ++poll_div >= 5) {
            poll_div = 0;
            uint8_t ec = 0;
            tca_read(REG_KEY_LCK_EC, &ec);
            int n = ec & 0x0F;
            if (n && !pbint) ESP_LOGW(TAG, "%d events but PBINT is not LOW", n);
            if (pbint && !pbint_seen) {
                pbint_seen = true;
                ESP_LOGI(TAG, "PBINT (GPIO%d) seen LOW: interrupt line OK", PIN_PBINT);
            }
            for (; n > 0; n--) {
                uint8_t ev;
                if (tca_read(REG_KEY_EVENT, &ev) == ESP_OK && ev) handle_event(ev);
            }
            tca_write(REG_INT_STAT, 0x01);  // clear K_INT
        }

        bool boot = gpio_get_level(PIN_BOOT);
        if (!boot && boot_prev) reset_progress();
        boot_prev = boot;

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
