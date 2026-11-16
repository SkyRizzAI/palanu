#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "hhv2_keymap.h"
#include "board.h"
#include "kb.h"

static const char *TAG = "kb";

#define PIN_SCL   12
#define PIN_SDA   13
#define PIN_PBRST 6
#define PIN_PBINT 7

#define TCA_ADDR       0x34
#define REG_CFG        0x01
#define REG_INT_STAT   0x02
#define REG_KEY_LCK_EC 0x03
#define REG_KEY_EVENT  0x04
#define REG_KP_GPIO1   0x1D
#define REG_KP_GPIO2   0x1E
#define REG_KP_GPIO3   0x1F

static i2c_master_dev_handle_t tca;
static bool shift_l, shift_r;

#define QN 16
static uint32_t q_key[QN];
static bool q_pressed[QN];
static int q_head, q_tail;

static esp_err_t tca_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(tca, b, 2, 50);
}

static esp_err_t tca_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(tca, &reg, 1, val, 1, 50);
}

static uint32_t to_lv_key(uint16_t k)
{
    bool shift = shift_l || shift_r;
    if (k >= 'A' && k <= 'Z') return shift ? k : (k - 'A' + 'a');
    switch (k) {
    case '.': return shift ? '$' : '.';
    case HHV2_KEY_SPACE: return ' ';
    case HHV2_KEY_ENTER: return LV_KEY_ENTER;
    case HHV2_KEY_DEL: return LV_KEY_BACKSPACE;
    case HHV2_KEY_ESC: return LV_KEY_ESC;
    case HHV2_KEY_TAB: return LV_KEY_NEXT;
    default: return 0;  // fn, ctrl, alt, Nm, cycle: not used yet
    }
}

static void push(uint32_t key, bool pressed)
{
    int next = (q_head + 1) % QN;
    if (next == q_tail) return;  // full, drop
    q_key[q_head] = key;
    q_pressed[q_head] = pressed;
    q_head = next;
}

static void poll_fifo(void)
{
    if (gpio_get_level(PIN_PBINT) != 0) return;  // no event
    uint8_t ec = 0;
    if (tca_read(REG_KEY_LCK_EC, &ec) != ESP_OK) return;
    for (int n = ec & 0x0F; n > 0; n--) {
        uint8_t ev;
        if (tca_read(REG_KEY_EVENT, &ev) != ESP_OK || ev == 0) break;
        bool pressed = ev & 0x80;
        int code = ev & 0x7F;
        if (code < 1 || code > HHV2_KB_ROWS * HHV2_KB_COLS) continue;
        uint16_t k = HHV2_KEYMAP[(code - 1) / 10][(code - 1) % 10];
        if (k == HHV2_KEY_SHIFT_L) { shift_l = pressed; continue; }
        if (k == HHV2_KEY_SHIFT_R) { shift_r = pressed; continue; }
        uint32_t lk = to_lv_key(k);
        if (!lk) continue;
        if (pressed) board_click();
        push(lk, pressed);
    }
    tca_write(REG_INT_STAT, 0x01);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t last_key;
    static bool last_pressed;
    if (q_head == q_tail) poll_fifo();
    if (q_head != q_tail) {
        last_key = q_key[q_tail];
        last_pressed = q_pressed[q_tail];
        q_tail = (q_tail + 1) % QN;
    }
    data->key = last_key;
    data->state = last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = (q_head != q_tail);
}

lv_indev_t *kb_init(void)
{
    gpio_config_t out = {.pin_bit_mask = 1ULL << PIN_PBRST, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_PBRST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_PBRST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_PBINT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&in));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA_ADDR, .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &tca));

    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO1, 0x0F));
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO2, 0xFF));
    ESP_ERROR_CHECK(tca_write(REG_KP_GPIO3, 0x03));
    uint8_t ev;
    for (int i = 0; i < 16; i++) tca_read(REG_KEY_EVENT, &ev);
    ESP_ERROR_CHECK(tca_write(REG_INT_STAT, 0x1F));
    ESP_ERROR_CHECK(tca_write(REG_CFG, 0x01));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, read_cb);
    ESP_LOGI(TAG, "TCA8418 keyboard ready as the LVGL keypad");
    return indev;
}
