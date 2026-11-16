// Bring-up 03: graphics benchmark of the ILI9341 LCD (320x240, SPI) on the hhv2 board.
//
// Measures real FPS at several SPI clocks and several kinds of load:
//   FILL    : a full colour change every frame (purely the SPI bandwidth limit)
//   PLASMA  : a per-pixel effect computed by the CPU (CPU + SPI load)
//   SPRITES : 120 bouncing boxes over a gradient background
//   SCROLL  : large scrolling text
//   PARTIAL : only a small 96x96 area is sent (dirty-rect UI style)
//
// Double framebuffer in PSRAM (DMA straight from PSRAM). Core 0 draws the next
// frame while a task on core 1 sends the previous one.
// One SPI transaction on the S31 is at most 2^18 bits (32 KB), so one frame
// (150 KB) is sent in 5 slices of 48 rows each.
// The result is shown on the screen (top bar) and printed to serial.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "hhv2_lcd.h"  // only for the font and the colour macros

static const char *TAG = "gfx";

#define PIN_SCK     14
#define PIN_MOSI    18
#define PIN_MISO    19
#define PIN_LCD_CS  15
#define PIN_LCD_DC  16
#define PIN_LCD_RST 17
#define PIN_LCD_BL  10
#define PIN_SD_CS   5

#define W 320
#define H 240
#define FB_BYTES (W * H * 2)
#define LCD_HOST SPI2_HOST
#define BAND_ROWS 48                       // 320*48*2 = 30 KB, under the 32 KB limit
#define BAND_BYTES (W * BAND_ROWS * 2)

#define SW(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
#define RGB(r, g, b) HHV2_RGB(r, g, b)

static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t trans_done;   // one slice finished (from the ISR)
static SemaphoreHandle_t frame_done;   // one flush job finished (from the flush task)
static QueueHandle_t flush_q;
typedef struct { const uint16_t *buf; int x, y, w, h; } flush_job_t;
static uint16_t *fb[2];
static uint16_t *bgfb;     // gradient background for the SPRITES demo
static uint16_t *small;    // small-area buffer for the PARTIAL demo
static bool fb_in_psram;

// ---------------------------------------------------------------- LCD
static bool on_trans_done(esp_lcd_panel_io_handle_t h, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(trans_done, &woken);
    return woken == pdTRUE;
}

static void lcd_bus_init(void)
{
    gpio_config_t g = {.pin_bit_mask = (1ULL << PIN_SD_CS) | (1ULL << PIN_LCD_BL), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&g));
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_level(PIN_LCD_BL, 0);
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK, .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BAND_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    trans_done = xSemaphoreCreateBinary();
    frame_done = xSemaphoreCreateBinary();
    xSemaphoreGive(frame_done);
    flush_q = xQueueCreate(1, sizeof(flush_job_t));
}

// Task on core 1: send the area in slices <= 32 KB, wait for each slice to finish
static void flush_task(void *arg)
{
    flush_job_t j;
    while (true) {
        xQueueReceive(flush_q, &j, portMAX_DELAY);
        int rows = BAND_BYTES / (j.w * 2);
        for (int y = 0; y < j.h; y += rows) {
            int h = (j.h - y) < rows ? (j.h - y) : rows;
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, j.x, j.y + y, j.x + j.w, j.y + y + h, j.buf + y * j.w));
            xSemaphoreTake(trans_done, portMAX_DELAY);
        }
        xSemaphoreGive(frame_done);
    }
}

// (Re)create the panel at a given SPI clock
static void lcd_open(int clk_hz)
{
    if (panel) {
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(io);
        panel = NULL;
    }
    esp_lcd_panel_io_spi_config_t ioc = {
        .cs_gpio_num = PIN_LCD_CS, .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0, .pclk_hz = clk_hz, .trans_queue_depth = 4,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = on_trans_done,
        .flags.psram_dma_direct = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &ioc, &io));
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &pc, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
}

// Wait for the previous flush job to finish, then hand this area to the flush task
static int64_t wait_us_total;
static void flush_async(int x, int y, int w, int h, const uint16_t *buf)
{
    int64_t t = esp_timer_get_time();
    xSemaphoreTake(frame_done, portMAX_DELAY);
    wait_us_total += esp_timer_get_time() - t;
    flush_job_t j = {buf, x, y, w, h};
    xQueueSend(flush_q, &j, portMAX_DELAY);
}

static void flush_wait(void)
{
    xSemaphoreTake(frame_done, portMAX_DELAY);
    xSemaphoreGive(frame_done);
}

// ---------------------------------------------------------------- drawing into the framebuffer
static void fb_rect(uint16_t *f, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t s = SW(c);
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *p = f + yy * W + x;
        for (int i = 0; i < w; i++) p[i] = s;
    }
}

static void fb_text(uint16_t *f, int x, int y, const char *s, int scale, uint16_t c)
{
    uint16_t sc = SW(c);
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = hhv2_font_glyph(s[i]);
        int cx = x + i * 6 * scale;
        if (cx >= W) break;
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++) {
                if (!(g[r] & (0x10 >> col))) continue;
                for (int dy = 0; dy < scale; dy++) {
                    int py = y + r * scale + dy;
                    if (py < 0 || py >= H) continue;
                    for (int dx = 0; dx < scale; dx++) {
                        int px = cx + col * scale + dx;
                        if (px >= 0 && px < W) f[py * W + px] = sc;
                    }
                }
            }
    }
}

static void overlay(uint16_t *f, int mhz, const char *demo, float fps)
{
    char line[48];
    fb_rect(f, 0, 0, W, 18, RGB(0, 0, 0));
    snprintf(line, sizeof(line), "%dMHZ %s %.1f FPS", mhz, demo, fps);
    fb_text(f, 3, 2, line, 2, RGB(255, 220, 0));
}

// ---------------------------------------------------------------- demo
static uint8_t sinlut[256];
static uint16_t palette[256];

static void tables_init(void)
{
    for (int i = 0; i < 256; i++) {
        sinlut[i] = (uint8_t)(127.5f + 127.5f * sinf(i * 2.0f * (float)M_PI / 256.0f));
        float t = i / 256.0f * 2.0f * (float)M_PI;
        int r = (int)(127.5f + 127.5f * sinf(t));
        int g = (int)(127.5f + 127.5f * sinf(t + 2.094f));
        int b = (int)(127.5f + 127.5f * sinf(t + 4.188f));
        palette[i] = SW(RGB(r, g, b));
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            bgfb[y * W + x] = SW(RGB(x * 80 / W, 20 + y * 60 / H, 60 + y * 120 / H));
}

static void draw_fill(uint16_t *f, int t)
{
    static const uint16_t cols[] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 255), RGB(0, 0, 0)};
    fb_rect(f, 0, 0, W, H, cols[t % 5]);
}

static void draw_plasma(uint16_t *f, int t)
{
    for (int y = 0; y < H; y++) {
        uint16_t *p = f + y * W;
        int a = sinlut[(y * 2 + t) & 255];
        int b0 = (y + t * 3);
        for (int x = 0; x < W; x++) {
            int v = sinlut[(x * 3 + t * 2) & 255] + a + sinlut[((x + b0) * 2) & 255];
            p[x] = palette[(v / 3 + t) & 255];
        }
    }
}

#define NSPR 120
typedef struct { int16_t x, y, dx, dy; uint16_t c; } spr_t;
static spr_t spr[NSPR];

static void sprites_init(void)
{
    for (int i = 0; i < NSPR; i++) {
        spr[i].x = (i * 37) % (W - 16);
        spr[i].y = 20 + (i * 53) % (H - 36);
        spr[i].dx = 1 + i % 4;
        spr[i].dy = 1 + (i / 4) % 3;
        if (i & 1) spr[i].dx = -spr[i].dx;
        spr[i].c = palette[(i * 23) & 255];
    }
}

static void draw_sprites(uint16_t *f, int t)
{
    memcpy(f, bgfb, FB_BYTES);
    for (int i = 0; i < NSPR; i++) {
        spr_t *s = &spr[i];
        s->x += s->dx; s->y += s->dy;
        if (s->x < 0 || s->x > W - 16) { s->dx = -s->dx; s->x += 2 * s->dx; }
        if (s->y < 18 || s->y > H - 16) { s->dy = -s->dy; s->y += 2 * s->dy; }
        for (int yy = 0; yy < 16; yy++) {
            uint16_t *p = f + (s->y + yy) * W + s->x;
            for (int xx = 0; xx < 16; xx++) p[xx] = s->c;
        }
    }
}

static void draw_scroll(uint16_t *f, int t)
{
    const char *msg = "PALANU OS ON ESP32-S31 HHV2 - ";
    int tw = (int)strlen(msg) * 6 * 6;
    fb_rect(f, 0, 0, W, H, RGB(8, 10, 30));
    int off = (t * 6) % tw;
    for (int x = -off; x < W; x += tw) fb_text(f, x, 90, msg, 6, RGB(0, 230, 255));
    for (int x = -((t * 3) % (tw / 2)); x < W; x += tw / 2) fb_text(f, x, 170, msg, 3, RGB(255, 120, 200));
}

typedef struct { const char *name; void (*draw)(uint16_t *, int); } demo_t;
static const demo_t DEMOS[] = {
    {"FILL", draw_fill},
    {"PLASMA", draw_plasma},
    {"SPRITES", draw_sprites},
    {"SCROLL", draw_scroll},
};
#define NDEMO (sizeof(DEMOS) / sizeof(DEMOS[0]))

typedef struct { float fps, render_ms, wait_ms; } result_t;

#define DEMO_US (4 * 1000 * 1000)

static result_t run_full(int mhz, const demo_t *d)
{
    int cur = 0, frames = 0;
    int64_t render_us = 0;
    float shown_fps = 0;
    wait_us_total = 0;
    int64_t t0 = esp_timer_get_time(), last = t0;
    int last_frames = 0;
    while (esp_timer_get_time() - t0 < DEMO_US) {
        int64_t r0 = esp_timer_get_time();
        d->draw(fb[cur], frames);
        overlay(fb[cur], mhz, d->name, shown_fps);
        render_us += esp_timer_get_time() - r0;
        flush_async(0, 0, W, H, fb[cur]);
        cur ^= 1;
        frames++;
        int64_t now = esp_timer_get_time();
        if (now - last >= 500000) {
            shown_fps = (frames - last_frames) * 1e6f / (now - last);
            last = now;
            last_frames = frames;
        }
    }
    flush_wait();
    float secs = (esp_timer_get_time() - t0) / 1e6f;
    result_t r = {frames / secs, render_us / 1000.0f / frames, wait_us_total / 1000.0f / frames};
    return r;
}

// Only the 96x96 box is sent every frame, like a UI that updates part of the screen
static result_t run_partial(int mhz)
{
    const int B = 96, box = 64;
    // static background sent once
    fb_rect(fb[0], 0, 0, W, H, RGB(8, 10, 30));
    overlay(fb[0], mhz, "PARTIAL 96X96", 0);
    flush_async(0, 0, W, H, fb[0]);
    flush_wait();

    int frames = 0, x = 0, y = 30, dx = 3, dy = 2;
    wait_us_total = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < DEMO_US) {
        x += dx; y += dy;
        if (x < 0 || x > W - B) { dx = -dx; x += 2 * dx; }
        if (y < 20 || y > H - B) { dy = -dy; y += 2 * dy; }
        // fill the small buffer: background + a coloured box in the middle
        flush_wait();  // there is only one small buffer, wait for the previous send
        uint16_t bg = SW(RGB(8, 10, 30)), fg = palette[(frames * 2) & 255];
        for (int yy = 0; yy < B; yy++)
            for (int xx = 0; xx < B; xx++) {
                bool in = xx >= (B - box) / 2 && xx < (B + box) / 2 && yy >= (B - box) / 2 && yy < (B + box) / 2;
                small[yy * B + xx] = in ? fg : bg;
            }
        flush_async(x, y, B, B, small);
        frames++;
    }
    flush_wait();
    float secs = (esp_timer_get_time() - t0) / 1e6f;
    result_t r = {frames / secs, 0, 0};
    return r;
}

// ---------------------------------------------------------------- main
static const int CLOCKS_MHZ[] = {40, 60, 80};
#define NCLK (sizeof(CLOCKS_MHZ) / sizeof(CLOCKS_MHZ[0]))

static void summary(result_t res[NCLK][NDEMO + 1])
{
    uint16_t *f = fb[0];
    char line[64];
    fb_rect(f, 0, 0, W, H, RGB(8, 10, 30));
    fb_text(f, 4, 4, "BENCHMARK RESULT (FPS)", 2, RGB(255, 220, 0));
    fb_text(f, 4, 26, fb_in_psram ? "FRAMEBUFFER: PSRAM" : "FRAMEBUFFER: INTERNAL RAM", 1, RGB(180, 180, 180));
    int y = 44;
    fb_text(f, 4, y, "MHZ  FILL PLSM SPRT SCRL PART", 2, RGB(0, 230, 255));
    for (int c = 0; c < (int)NCLK; c++) {
        y += 22;
        snprintf(line, sizeof(line), "%3d %5.0f%5.0f%5.0f%5.0f%5.0f", CLOCKS_MHZ[c], res[c][0].fps, res[c][1].fps,
                 res[c][2].fps, res[c][3].fps, res[c][4].fps);
        fb_text(f, 4, y, line, 2, RGB(230, 230, 230));
    }
    y += 30;
    for (int c = 0; c < (int)NCLK; c++) {
        snprintf(line, sizeof(line), "THEORETICAL LIMIT %dMHZ: %.1f FPS FULL", CLOCKS_MHZ[c], CLOCKS_MHZ[c] * 1e6f / (W * H * 16));
        fb_text(f, 4, y, line, 1, RGB(180, 180, 180));
        y += 12;
    }
    fb_text(f, 4, H - 12, "REPEATS IN 15 SECONDS...", 1, RGB(180, 180, 180));
    flush_async(0, 0, W, H, f);
    flush_wait();
}

void app_main(void)
{
    ESP_LOGI(TAG, "PSRAM: %s, size %u KB", esp_psram_is_initialized() ? "present" : "absent",
             (unsigned)(esp_psram_is_initialized() ? esp_psram_get_size() / 1024 : 0));

    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    fb[0] = heap_caps_aligned_alloc(64, FB_BYTES, caps);
    fb[1] = heap_caps_aligned_alloc(64, FB_BYTES, caps);
    bgfb = heap_caps_aligned_alloc(64, FB_BYTES, MALLOC_CAP_SPIRAM);
    fb_in_psram = fb[0] && fb[1] && bgfb;
    if (!fb_in_psram) {
        ESP_LOGW(TAG, "PSRAM not available, framebuffer in internal RAM");
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
        fb[0] = heap_caps_malloc(FB_BYTES, caps);
        fb[1] = heap_caps_malloc(FB_BYTES, caps);
        bgfb = fb[1];  // not enough RAM for a separate background
    }
    small = heap_caps_malloc(96 * 96 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(fb[0] && fb[1] && small);
    ESP_LOGI(TAG, "Framebuffer 2x %d KB in %s. Free internal heap: %u KB", FB_BYTES / 1024,
             fb_in_psram ? "PSRAM" : "internal", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    lcd_bus_init();
    xTaskCreatePinnedToCore(flush_task, "flush", 4096, NULL, 10, NULL, 1);
    tables_init();
    sprites_init();

    static result_t res[NCLK][NDEMO + 1];
    while (true) {
        for (int c = 0; c < (int)NCLK; c++) {
            int mhz = CLOCKS_MHZ[c];
            lcd_open(mhz * 1000 * 1000);
            gpio_set_level(PIN_LCD_BL, 1);
            for (int d = 0; d < (int)NDEMO; d++) {
                res[c][d] = run_full(mhz, &DEMOS[d]);
                ESP_LOGI(TAG, "%2d MHz %-8s %6.1f fps  render %5.2f ms  SPI wait %5.2f ms", mhz, DEMOS[d].name,
                         res[c][d].fps, res[c][d].render_ms, res[c][d].wait_ms);
            }
            res[c][NDEMO] = run_partial(mhz);
            ESP_LOGI(TAG, "%2d MHz %-8s %6.1f fps  (area 96x96)", mhz, "PARTIAL", res[c][NDEMO].fps);
        }
        summary(res);
        ESP_LOGI(TAG, "One round finished, summary on the screen");
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
